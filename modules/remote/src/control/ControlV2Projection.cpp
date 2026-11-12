#include "control/ControlV2Client.h"
#include "control/ControlV2Content.h"
#include <QDateTime>
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
QJsonObject reject(const QString& kind, const QJsonObject& entry, const QString& reason,
                   const QString& detail = {}) {
    QJsonObject row{
        {"kind", kind},
        {"id", entry.value(kind == "worktree" ? "worktreeId" : "id").toString().left(256)},
        {"machineId", entry.value("machineId").toString().left(256)},
        {"reason", reason}};
    if (!detail.isEmpty())
        row.insert("detail", detail.left(512));
    return row;
}
// A stable reason for a failed key verification; the message itself goes in `detail`.
QString keyFailure(const QString& error) {
    if (error == QLatin1String(control_v2::noSessionKeyGrant))
        return "session_key_pending";
    if (error == QLatin1String(control_v2::hostNotPaired))
        return "host_key_not_pinned";
    return "verification_failed";
}
// The fields the host seals for each conversation (CloudControlClient::sessionSnapshots). Only
// these are shown; the relay's plaintext routing copy never reaches the projection.
QString verifiedMetadata(const QJsonObject& metadata, QJsonObject* row) {
    if (!boundedText(metadata.value("title"), 1024) || !boundedText(metadata.value("state"), 64))
        return "host_upgrade_required";
    if (!boundedText(metadata.value("branch"), 1024) ||
        !boundedText(metadata.value("machineName"), 512) ||
        !metadata.value("worktreeExists").isBool() ||
        !boundedText(metadata.value("repositoryId"), 128) ||
        !boundedText(metadata.value("worktreeId"), 128) ||
        !boundedText(metadata.value("repositoryLabel"), 512) ||
        !boundedText(metadata.value("worktreeLabel"), 512) ||
        !boundedText(metadata.value("lastTurnStartedAt"), 64, false) ||
        !boundedText(metadata.value("lastTurnOutcome"), 32, false) ||
        !boundedText(metadata.value("number"), 16, false))
        return "invalid_session_metadata";
    for (const auto* key : {"title", "state", "branch", "machineName", "worktreeExists",
                            "repositoryId", "repositoryLabel", "worktreeId", "worktreeLabel"})
        row->insert(QLatin1String(key), metadata.value(QLatin1String(key)));
    row->insert("updatedAt", metadata.value("lastTurnStartedAt").toString());
    for (const auto* key : {"lastTurnOutcome", "number", "awaitingReply"})
        if (metadata.contains(QLatin1String(key)))
            row->insert(QLatin1String(key), metadata.value(QLatin1String(key)));
    return {};
}
} // namespace

struct ControlV2Client::ProjectionState {
    QJsonArray sessions;
    QJsonArray worktrees;
    qsizetype next{0};
    QJsonArray conversations;
    QJsonArray rejected;
    // Index worktrees no verified conversation covers: at most verifyBound uncached ones are
    // queued for a host-signed worktree.configuration.read (reported in `pending`), and those
    // whose cached verdict matched are shown.
    int verifyBound{0};
    QJsonArray pending;
    QList<QJsonObject> hostVerified;
    quint64 epoch{0};
    Callback done;
};

bool ControlV2Client::hostPinned(const QString& machineId) const {
    if (machineId.isEmpty())
        return false;
    // A client paired before pins adopts its host through an event read (see senderKey); until
    // then no relay-named machine is trusted here.
    if (!hostPinsStored_)
        return false;
    const auto prefix = machineId + '/';
    for (auto it = hostPins_.begin(); it != hostPins_.end(); ++it)
        if (it.key().startsWith(prefix))
            return true;
    return false;
}

void ControlV2Client::projection(Callback done) { projection({}, std::move(done)); }

void ControlV2Client::projection(const QJsonObject& options, Callback done) {
    auto state = std::make_shared<ProjectionState>();
    state->done = std::move(done);
    state->verifyBound = qBound(0, options.value("verify_worktrees").toInt(0), 100);
    state->epoch = epoch_;
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
    // Routing scope only: everything shown comes from the sealed metadata below. adapter,
    // lastEventCursor, currentTurnSequence and workspaceRevision are unsigned relay routing
    // values, not host-signed: they select what to fetch, and every event or page fetched with
    // them is verified on its own.
    QJsonObject row;
    for (const auto* key : {"id", "generation", "workspaceId", "machineId", "adapter",
                            "lastEventCursor", "currentTurnSequence", "workspaceRevision"})
        if (session.contains(QLatin1String(key)))
            row.insert(QLatin1String(key), session.value(QLatin1String(key)));
    if (!session.value("encryptedMetadata").isObject()) {
        // The relay sends null when the host sealed nothing; such a row cannot be verified.
        state->rejected.append(reject("conversation", session, "metadata_absent"));
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
        if (!error.isEmpty()) {
            // Includes a first heartbeat that arrives before its wrapped key: the next
            // projection shows the row once the host has granted it.
            state->rejected.append(reject("conversation", session, keyFailure(error), error));
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
    // The relay's worktree index is unsigned. A worktree is shown when a verified conversation
    // names it, or when the host signs its scope in a worktree.configuration.read result.
    QSet<QString> covered;
    for (const auto& value : std::as_const(state->conversations)) {
        const auto row = value.toObject();
        covered.insert(row.value("machineId").toString() + '\n' +
                       row.value("worktreeId").toString());
    }
    if (verdictEpoch_ != epoch_) {
        worktreeVerdicts_.clear();
        verifyQueue_.clear();
        verifying_.clear();
        verdictEpoch_ = epoch_;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    int scheduled = 0;
    QSet<QString> seen;
    for (const auto& value : std::as_const(state->worktrees)) {
        const auto entry = value.toObject();
        const auto machine = entry.value("machineId").toString();
        const auto key = machine + '\n' + entry.value("worktreeId").toString();
        if (seen.contains(key) || covered.contains(key))
            continue;
        seen.insert(key);
        if (!hostPinned(machine)) {
            state->rejected.append(reject("worktree", entry, "unpinned_host"));
            continue;
        }
        const auto verdict = worktreeVerdicts_.constFind(worktreeVerdictKey(entry));
        const auto known =
            verdict != worktreeVerdicts_.cend() && (verdict->until == 0 || verdict->until > now);
        if (entry.value("status").toString("active") != "active") {
            state->rejected.append(reject("worktree", entry, "unsigned_index_entry"));
        } else if (known) {
            if (verdict->reason.isEmpty())
                state->hostVerified.append(entry);
            else
                state->rejected.append(reject("worktree", entry, verdict->reason, verdict->detail));
        } else if (scheduled < state->verifyBound) {
            // Lazy: the listing never waits for the host; worktreesVerified() announces the
            // verdict, and the next listing shows or rejects the entry.
            ++scheduled;
            state->pending.append(reject("worktree", entry, "verification_pending"));
            queueWorktreeVerification(entry);
        } else {
            state->rejected.append(reject("worktree", entry, "unsigned_index_entry"));
        }
    }
    pumpWorktreeVerification();
    assembleProjection(state);
}

QString ControlV2Client::worktreeVerdictKey(const QJsonObject& entry) {
    // Once per catalog scope: any change of ids, labels or revisions is a new verification.
    QStringList parts;
    for (const auto* key : {"workspaceId", "machineId", "repositoryId", "worktreeId",
                            "repositoryLabel", "worktreeLabel"})
        parts.append(entry.value(QLatin1String(key)).toString());
    for (const auto* key : {"catalogRevision", "lifecycleRevision"})
        parts.append(QString::number(entry.value(QLatin1String(key)).toInteger(0)));
    return parts.join('\n');
}

void ControlV2Client::queueWorktreeVerification(const QJsonObject& entry) {
    const auto key = worktreeVerdictKey(entry);
    if (verifying_.contains(key))
        return;
    verifying_.insert(key);
    verifyQueue_.append(entry);
}

void ControlV2Client::pumpWorktreeVerification() {
    while (verifyInFlight_ < verifyConcurrency_ && !verifyQueue_.isEmpty()) {
        const auto entry = verifyQueue_.takeFirst();
        const auto key = worktreeVerdictKey(entry);
        const auto epoch = epoch_;
        ++verifyInFlight_;
        QJsonObject intent{{"kind", "worktree.configuration.read"}};
        for (const auto* field : {"workspaceId", "machineId", "repositoryId", "worktreeId"})
            intent.insert(QLatin1String(field), entry.value(QLatin1String(field)));
        intent.insert("expectedCatalogRevision", entry.value("catalogRevision").toInteger(0));
        intent.insert("expectedLifecycleRevision", entry.value("lifecycleRevision").toInteger(0));
        awaitWorktreeCommand(
            intent, {}, verifyTimeoutMilliseconds_,
            [this, entry, key, epoch](const QJsonValue& value, const QString& error) {
                --verifyInFlight_;
                verifying_.remove(key);
                if (epoch == epoch_) {
                    WorktreeVerdict verdict;
                    if (!error.isEmpty()) {
                        verdict.detail = error;
                        if (error.contains("forbidden")) {
                            verdict.reason = QStringLiteral("worktree_not_granted");
                        } else {
                            // No answer: tried again in a later listing, not on every one.
                            verdict.reason = QStringLiteral("verification_failed");
                            verdict.until =
                                QDateTime::currentMSecsSinceEpoch() + verifyRetryMilliseconds_;
                        }
                    } else {
                        // The host's own catalog scope for the worktree, inside its signed
                        // result. The host keeps no catalog revision, so only ids and labels
                        // are compared (ADR 0119); the revisions are part of the cache key.
                        const auto echo = value.toObject()
                                              .value("result")
                                              .toObject()
                                              .value("worktree")
                                              .toObject();
                        bool same = !echo.isEmpty();
                        for (const auto* field :
                             {"repositoryId", "worktreeId", "repositoryLabel", "worktreeLabel"})
                            same &= echo.value(QLatin1String(field)).toString() ==
                                        entry.value(QLatin1String(field)).toString() &&
                                    boundedText(echo.value(QLatin1String(field)), 512);
                        if (!same)
                            verdict.reason = QStringLiteral("verification_failed");
                    }
                    worktreeVerdicts_.insert(key, verdict);
                    emit worktreesVerified();
                }
                pumpWorktreeVerification();
            });
    }
}

void ControlV2Client::assembleProjection(const std::shared_ptr<ProjectionState>& state) {
    QHash<QString, QJsonObject> indexed;
    for (const auto& value : std::as_const(state->worktrees)) {
        const auto entry = value.toObject();
        const auto key =
            entry.value("machineId").toString() + '\n' + entry.value("worktreeId").toString();
        if (!indexed.contains(key))
            indexed.insert(key, entry);
    }
    QJsonArray repositories;
    QHash<QString, qsizetype> repositoryIndex;
    QSet<QString> shown;
    const auto add = [&](const QString& machine, const QJsonValue& repositoryId,
                         const QJsonValue& repositoryLabel, QJsonObject item) {
        const auto key = machine + '\n' + repositoryId.toString();
        if (!repositoryIndex.contains(key)) {
            repositoryIndex.insert(key, repositories.size());
            repositories.append(QJsonObject{{"machineId", machine},
                                            {"repositoryId", repositoryId},
                                            {"label", repositoryLabel},
                                            {"worktrees", QJsonArray{}}});
        }
        auto group = repositories.at(repositoryIndex.value(key)).toObject();
        auto worktrees = group.value("worktrees").toArray();
        worktrees.append(item);
        group.insert("worktrees", worktrees);
        repositories.replace(repositoryIndex.value(key), group);
    };
    for (const auto& value : std::as_const(state->conversations)) {
        const auto row = value.toObject();
        const auto machine = row.value("machineId").toString();
        const auto worktree = row.value("worktreeId").toString();
        if (worktree.isEmpty() || shown.contains(machine + '\n' + worktree))
            continue;
        shown.insert(machine + '\n' + worktree);
        QJsonObject item{{"worktreeId", worktree},
                         {"workspaceId", row.value("workspaceId")},
                         {"label", row.value("worktreeLabel")},
                         {"branch", row.value("branch")},
                         {"exists", row.value("worktreeExists")},
                         {"verified", "conversation"}};
        const auto entry = indexed.value(machine + '\n' + worktree);
        if (entry.value("repositoryId") == row.value("repositoryId"))
            for (const auto* revision : {"catalogRevision", "lifecycleRevision"})
                if (entry.value(QLatin1String(revision)).isDouble())
                    item.insert(QLatin1String(revision), entry.value(QLatin1String(revision)));
        add(machine, row.value("repositoryId"), row.value("repositoryLabel"), item);
    }
    for (const auto& entry : std::as_const(state->hostVerified))
        add(entry.value("machineId").toString(), entry.value("repositoryId"),
            entry.value("repositoryLabel"),
            {{"worktreeId", entry.value("worktreeId")},
             {"workspaceId", entry.value("workspaceId")},
             {"label", entry.value("worktreeLabel")},
             {"exists", true},
             {"catalogRevision", entry.value("catalogRevision")},
             {"lifecycleRevision", entry.value("lifecycleRevision")},
             {"verified", "host_read"}});
    const auto done = std::move(state->done);
    done(QJsonObject{{"repositories", repositories},
                     {"conversations", state->conversations},
                     {"rejected", state->rejected},
                     {"pending", state->pending}},
         {});
}
} // namespace ketplus::remote
