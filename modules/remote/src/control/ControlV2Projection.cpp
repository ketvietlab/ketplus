#include "control/ControlV2Client.h"
#include "control/ControlV2Content.h"
#include <QJsonDocument>
#include <QSet>

// Host-verified projection of the session and worktree indexes for a paired Desktop.
namespace ketplus::remote {
using control_v2::validContent;
namespace {
bool boundedText(const QJsonValue& value, qsizetype maximum, bool required = true) {
    if (value.isUndefined() || value.isNull())
        return !required;
    return value.isString() && value.toString().toUtf8().size() <= maximum;
}
QJsonObject reject(const QString& kind, const QJsonObject& entry, const QString& reason) {
    return {{"kind", kind},
            {"id", entry.value(kind == "worktree" ? "worktreeId" : "id").toString().left(256)},
            {"machineId", entry.value("machineId").toString().left(256)},
            {"reason", reason}};
}
// The fields the host seals for each conversation (CloudControlClient::sessionSnapshots).
QString verifiedMetadata(const QJsonObject& metadata, QJsonObject* row) {
    if (!boundedText(metadata.value("branch"), 1024) ||
        !boundedText(metadata.value("machineName"), 512) ||
        !metadata.value("worktreeExists").isBool() ||
        !boundedText(metadata.value("title"), 1024, false) ||
        !boundedText(metadata.value("state"), 64, false) ||
        !boundedText(metadata.value("lastTurnStartedAt"), 64, false) ||
        !boundedText(metadata.value("lastTurnOutcome"), 32, false) ||
        !boundedText(metadata.value("number"), 16, false))
        return "invalid_session_metadata";
    // Daemons that seal the title and state are authoritative over the relay's plaintext copy.
    for (const auto* key : {"title", "state"})
        if (metadata.value(QLatin1String(key)).isString())
            row->insert(QLatin1String(key), metadata.value(QLatin1String(key)));
    row->insert("branch", metadata.value("branch"));
    row->insert("machineName", metadata.value("machineName"));
    row->insert("worktreeExists", metadata.value("worktreeExists"));
    row->insert("updatedAt", metadata.value("lastTurnStartedAt").toString());
    for (const auto* key : {"lastTurnOutcome", "number", "awaitingReply"})
        if (metadata.contains(QLatin1String(key)))
            row->insert(QLatin1String(key), metadata.value(QLatin1String(key)));
    row->insert("metadata", "verified");
    return {};
}
} // namespace

struct ControlV2Client::ProjectionState {
    QJsonArray sessions;
    QJsonArray worktrees;
    qsizetype next{0};
    QJsonArray conversations;
    QJsonArray rejected;
    Callback done;
};

bool ControlV2Client::hostPinned(const QString& machineId) const {
    if (machineId.isEmpty())
        return false;
    // A client paired before pins adopts its host on the first key read (see senderKey).
    if (!hostPinsStored_)
        return credential_.contains("deviceId");
    const auto prefix = machineId + '/';
    for (auto it = hostPins_.begin(); it != hostPins_.end(); ++it)
        if (it.key().startsWith(prefix))
            return true;
    return false;
}

void ControlV2Client::projection(Callback done) {
    auto state = std::make_shared<ProjectionState>();
    state->done = std::move(done);
    listSessions([this, state](const QJsonValue& sessions, const QString& error) {
        if (!error.isEmpty() || !sessions.isArray()) {
            state->done({}, error.isEmpty() ? "Invalid session index." : error);
            return;
        }
        state->sessions = sessions.toArray();
        listWorktrees([this, state](const QJsonValue& worktrees, const QString& failure) {
            if (!failure.isEmpty() || !worktrees.isArray()) {
                state->done({}, failure.isEmpty() ? "Invalid worktree index." : failure);
                return;
            }
            state->worktrees = worktrees.toArray();
            QSet<QString> ids;
            for (const auto& value : std::as_const(state->sessions)) {
                const auto id = value.toObject().value("id").toString();
                if (id.isEmpty())
                    continue;
                if (ids.contains(id)) {
                    // Two rows for one conversation: the relay index is inconsistent.
                    state->done({}, "Duplicate conversation in the session index.");
                    return;
                }
                ids.insert(id);
            }
            projectNext(state);
        });
    });
}

void ControlV2Client::projectNext(const std::shared_ptr<ProjectionState>& state) {
    if (state->next >= state->sessions.size()) {
        finishProjection(state);
        return;
    }
    const auto session = state->sessions.at(state->next++).toObject();
    const auto id = session.value("id").toString();
    const auto generation = session.value("generation").toInteger(-1);
    const auto machine = session.value("machineId").toString();
    const auto lastCursor = session.value("lastEventCursor").toInteger(-1);
    if (id.isEmpty() || id.size() > 256 || generation < 1 || machine.isEmpty() ||
        session.value("workspaceId").toString().isEmpty() || lastCursor < 0 ||
        !boundedText(session.value("title"), 1024) || !boundedText(session.value("state"), 64)) {
        state->rejected.append(reject("conversation", session, "invalid_index_entry"));
        projectNext(state);
        return;
    }
    if (!hostPinned(machine)) {
        state->rejected.append(reject("conversation", session, "unpinned_host"));
        projectNext(state);
        return;
    }
    QJsonObject row;
    for (const auto* key :
         {"id", "generation", "workspaceId", "machineId", "adapter", "title", "state",
          "lastEventCursor", "currentTurnSequence", "repositoryId", "repositoryLabel", "worktreeId",
          "worktreeLabel", "workspaceLabel", "workspaceRevision", "archivedAt"})
        if (session.contains(QLatin1String(key)))
            row.insert(QLatin1String(key), session.value(QLatin1String(key)));
    if (!session.contains("encryptedMetadata")) {
        // Daemons before sealed metadata: only the relay's routing copy exists.
        row.insert("metadata", "absent");
        state->conversations.append(row);
        projectNext(state);
        return;
    }
    const auto envelope = session.value("encryptedMetadata").toObject();
    const auto content = envelope.value("content").toObject();
    const auto cursor = envelope.value("cursor").toInteger(-1);
    const auto turn = envelope.value("turnSequence").toInteger(-1);
    if (cursor < 0 || cursor > lastCursor || turn < 0 ||
        content.value("senderDeviceId").toString() != machine ||
        !validContent(content, id, generation, 64 * 1024)) {
        state->rejected.append(reject("conversation", session, "invalid_session_metadata"));
        projectNext(state);
        return;
    }
    const QJsonObject scope{{"id", id},
                            {"generation", generation},
                            {"workspaceId", session.value("workspaceId")},
                            {"machineId", machine}};
    sessionKeys(scope, [this, state, session, row, content, cursor,
                        turn](const QByteArray& signing, const QString& error) mutable {
        if (error == QLatin1String(control_v2::noSessionKeyGrant)) {
            // A first heartbeat can arrive before the wrapped key: list the row, no sealed fields.
            row.insert("metadata", "pending");
            state->conversations.append(row);
            projectNext(state);
            return;
        }
        if (!error.isEmpty()) {
            state->rejected.append(reject("conversation", session, error));
            projectNext(state);
            return;
        }
        const auto id = session.value("id").toString();
        const auto generation = session.value("generation").toInteger();
        if (!signedByVerifiedKey(id, generation, content)) {
            state->rejected.append(reject("conversation", session, "host_key_changed"));
            projectNext(state);
            return;
        }
        QJsonArray fields{"ketplus",
                          2,
                          "session-metadata",
                          session.value("workspaceId"),
                          session.value("machineId"),
                          id,
                          generation,
                          cursor,
                          turn};
        for (const auto& field : control_v2::contentScope(content))
            fields.append(field);
        QString failure;
        const auto plaintext = crypto_.decrypt(content, daemon::CloudCrypto::canonical(fields),
                                               signing, id, generation, &failure);
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(plaintext, &parse);
        if (!failure.isEmpty() || plaintext.isEmpty() || parse.error != QJsonParseError::NoError ||
            !json.isObject())
            failure = "unauthenticated_session_metadata";
        else
            failure = verifiedMetadata(json.object(), &row);
        if (failure.isEmpty())
            state->conversations.append(row);
        else
            state->rejected.append(reject("conversation", session, failure));
        projectNext(state);
    });
}

void ControlV2Client::finishProjection(const std::shared_ptr<ProjectionState>& state) {
    // Branches are known only from verified conversation metadata, never from the index.
    QHash<QString, QString> branches;
    for (const auto& value : std::as_const(state->conversations)) {
        const auto row = value.toObject();
        const auto key =
            row.value("machineId").toString() + '\n' + row.value("worktreeId").toString();
        if (row.value("metadata") == "verified" && !branches.contains(key))
            branches.insert(key, row.value("branch").toString());
    }
    QJsonArray repositories;
    QHash<QString, qsizetype> repositoryIndex;
    QSet<QString> seen;
    for (const auto& value : std::as_const(state->worktrees)) {
        const auto entry = value.toObject();
        const auto machine = entry.value("machineId").toString();
        const auto repository = entry.value("repositoryId").toString();
        const auto worktree = entry.value("worktreeId").toString();
        if (machine.isEmpty() || repository.isEmpty() || repository.size() > 128 ||
            worktree.isEmpty() || worktree.size() > 128 ||
            !boundedText(entry.value("repositoryLabel"), 512, false) ||
            !boundedText(entry.value("worktreeLabel"), 512, false) ||
            seen.contains(machine + '\n' + worktree)) {
            state->rejected.append(reject("worktree", entry, "invalid_index_entry"));
            continue;
        }
        if (!hostPinned(machine)) {
            state->rejected.append(reject("worktree", entry, "unpinned_host"));
            continue;
        }
        seen.insert(machine + '\n' + worktree);
        const auto key = machine + '\n' + repository;
        if (!repositoryIndex.contains(key)) {
            repositoryIndex.insert(key, repositories.size());
            repositories.append(QJsonObject{{"machineId", machine},
                                            {"repositoryId", repository},
                                            {"label", entry.value("repositoryLabel").toString()},
                                            {"worktrees", QJsonArray{}}});
        }
        auto group = repositories.at(repositoryIndex.value(key)).toObject();
        auto worktrees = group.value("worktrees").toArray();
        QJsonObject item{{"worktreeId", worktree},
                         {"label", entry.value("worktreeLabel").toString()},
                         {"catalogRevision", entry.value("catalogRevision")},
                         {"lifecycleRevision", entry.value("lifecycleRevision")}};
        if (const auto branch = branches.constFind(machine + '\n' + worktree);
            branch != branches.cend())
            item.insert("branch", *branch);
        worktrees.append(item);
        group.insert("worktrees", worktrees);
        repositories.replace(repositoryIndex.value(key), group);
    }
    const auto done = std::move(state->done);
    done(QJsonObject{{"repositories", repositories},
                     {"conversations", state->conversations},
                     {"rejected", state->rejected}},
         {});
}
} // namespace ketplus::remote
