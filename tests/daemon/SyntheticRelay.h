#pragma once

#include "RestoreHost.h"
#include "control/ControlV2Content.h"
#include "control/RelayMachineBackend.h"

#include <QUrlQuery>

// A loopback relay whose replies are shaped like the deployed relay's DTOs (its session,
// worktree and command objects), with a synthetic host that decrypts each command, executes it
// once and signs its result. Shared by the Relay backend tests and the Desktop end-to-end tests.
namespace {
using ConnectionState = MachineBackend::ConnectionState;
const QString origin = "https://synthetic.invalid/native/ketplus";

QJsonObject sealedMetadata(CloudCrypto& host, const QString& id, int generation, int cursor,
                           const QJsonObject& metadata) {
    const auto keyId = QStringLiteral("%1:%2:1").arg(id).arg(generation);
    const auto aad =
        CloudCrypto::canonical({"ketplus", 2, "session-metadata", "workspace", "linux-host", id,
                                generation, cursor, 1, keyId, 1, "linux-host", 1});
    return {{"cursor", cursor},
            {"turnSequence", 1},
            {"content", host.encrypt(QJsonDocument(metadata).toJson(QJsonDocument::Compact), aad,
                                     id, generation, 1, "linux-host", 1)}};
}

// As the relay's worktreeDto for a heartbeat catalog entry (no sealed field).
QJsonObject worktreeDto(const QString& id, const QString& label) {
    return {{"workspaceId", "workspace"},
            {"machineId", "linux-host"},
            {"repositoryId", "repo-1"},
            {"worktreeId", id},
            {"repositoryLabel", "ketplus"},
            {"worktreeLabel", label},
            {"machineDisplayName", "builder"},
            {"catalogRevision", 0},
            {"lifecycleRevision", 0},
            {"status", "active"},
            {"updatedAt", "2026-11-12T08:00:00.000Z"}};
}

class SyntheticRelay {
  public:
    FixtureTransport& network;
    EncryptedFixture& fixture;
    QByteArray clientSigning;
    QByteArray clientAgreement;
    bool offline{false};
    int machinesStatus{200};
    // The next N command submissions reach the relay, but their answers are lost.
    int dropSubmitAnswers{0};
    // The host is away: commands stay queued.
    bool hostPaused{false};
    QHash<QString, QJsonObject> commands;
    QHash<QString, QByteArray> firstBodies;
    QHash<QString, int> submissions;
    // Submissions whose body differed from the first one with the same id.
    int conflicts{0};
    QHash<QString, int> executions;
    QHash<QString, QJsonObject> plaintexts;
    QSet<QString> ungranted;
    // Session command kinds the relay refuses with no error code.
    QSet<QString> refusedKinds;
    // Session command kinds the relay answers with this status and no error code (an outage).
    QHash<QString, int> unavailableKinds;
    // Worktree label the host signs, when it differs from the relay's index.
    QHash<QString, QString> hostLabels;
    QJsonArray worktrees;
    // The relay's sessionDto list; the first is "opaque-session", which alone has events.
    QJsonArray sessions;
    QJsonArray events;
    int commandReads{0};
    QHash<QString, int> eventPolls;
    // Answers transcript.restore_window for its session when set (a transcript read).
    RestoreHost* restore{nullptr};
    QHash<QString, int> readsOfCommand;
    QJsonObject metadata_;
    QJsonObject clientKeys_;
    // Runs while an events request is in flight, before its answer is delivered.
    std::function<void()> onEvents;
    // When set and it answers a non-empty object, the host's result for that command kind and
    // plaintext instead of the built-in one (the Desktop tests use the daemon's result shapes).
    std::function<QJsonObject(const QString& kind, const QJsonObject& plaintext)> results;

    SyntheticRelay(FixtureTransport& transport, EncryptedFixture& host, ControlV2Client& client)
        : network(transport), fixture(host) {
        const auto keys = client.identity().value("keys").toObject();
        clientKeys_ = keys;
        clientSigning = CloudCrypto::fromBase64Url(keys.value("signingPublicKey").toString());
        clientAgreement = CloudCrypto::fromBase64Url(keys.value("agreementPublicKey").toString());
        const QJsonObject metadata{{"branch", "feature/relay"},
                                   {"machineName", "builder"},
                                   {"worktreeExists", true},
                                   {"lastTurnStartedAt", "2026-11-12T08:00:00.000Z"},
                                   {"lastTurnOutcome", "completed"},
                                   {"title", "Sealed title"},
                                   {"state", "ready"},
                                   {"repositoryId", "repo-1"},
                                   {"repositoryLabel", "ketplus"},
                                   {"worktreeId", "wt-a"},
                                   {"worktreeLabel", "ketplus"}};
        metadata_ = metadata;
        // As the relay's sessionDto.
        sessions = QJsonArray{QJsonObject{
            {"id", "opaque-session"},
            {"generation", 3},
            {"workspaceId", "workspace"},
            {"machineId", "linux-host"},
            {"adapter", "codex"},
            {"title", "Relay title"},
            {"state", "ready"},
            {"lastEventCursor", 9},
            {"currentTurnSequence", 1},
            {"repositoryId", "repo-1"},
            {"worktreeId", "wt-a"},
            {"encryptedMetadata", sealedMetadata(fixture.host, "opaque-session", 3, 9, metadata)}}};
        network.replies.insert(prefix + "/v2/mobile/sessions", sessions);
        worktrees = {worktreeDto("wt-a", "ketplus"), worktreeDto("wt-empty", "empty")};
        network.handler = [this](const QNetworkRequest& request, const QByteArray& body) {
            return handle(request, body);
        };
    }

    // Another conversation in the same verified worktree, listed from the next read on.
    void addSession(const QString& id) {
        auto row = sessions.first().toObject();
        row.insert("id", id);
        row.insert("encryptedMetadata", sealedMetadata(fixture.host, id, 3, 9, metadata_));
        sessions.append(row);
        network.replies.insert(prefix + "/v2/mobile/sessions", sessions);
        network.replies.insert(
            prefix + "/v2/mobile/sessions/" + id + "/keys",
            QJsonArray{fixture.host.wrapSessionKey(
                id, 3, 1, {{"id", "remote-client"}, {"keyVersion", 1}, {"keys", clientKeys_}},
                "linux-host", 1)});
    }

    std::optional<std::pair<int, QJsonObject>> handle(const QNetworkRequest& request,
                                                      const QByteArray& bytes) {
        const auto path = request.url().path().mid(prefix.size());
        const auto method = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        if (offline) {
            network.nextFailure = QNetworkReply::ConnectionRefusedError;
            return std::pair{0, QJsonObject{}};
        }
        if (path == "/v2/machines") {
            if (network.replies.contains(prefix + path))
                return std::nullopt;
            return std::pair{machinesStatus, QJsonObject{{"code", "not_found"}}};
        }
        if (path == "/v2/mobile/worktrees") {
            network.replies.insert(prefix + path, worktrees);
            return std::nullopt;
        }
        const auto body = QJsonDocument::fromJson(bytes).object();
        if (restore != nullptr &&
            (path.startsWith("/v2/mobile/restore-requests/") ||
             (body.value("kind") == "transcript.restore_window" &&
              path == "/v2/mobile/sessions/" + restore->session + "/commands")))
            return restore->answer(request, bytes);
        if (method == "POST" && path == "/v2/mobile/workspaces/workspace/session-creates")
            return accept(body, bytes, true);
        const auto sessionPath = QStringLiteral("/v2/mobile/sessions/");
        const auto session =
            path.startsWith(sessionPath) ? path.mid(sessionPath.size()).section('/', 0, 0) : "";
        if (method == "POST" && !session.isEmpty() && path.endsWith("/commands"))
            return accept(body, bytes, false);
        if (path.startsWith("/v2/mobile/commands/"))
            return read(path.mid(QStringLiteral("/v2/mobile/commands/").size()));
        if (!session.isEmpty() && path.endsWith("/events")) {
            ++eventPolls[session];
            if (onEvents)
                onEvents();
            const auto after = QUrlQuery(request.url()).queryItemValue("afterCursor").toLongLong();
            QJsonArray page;
            qint64 next = after;
            for (const auto& event : std::as_const(events))
                if (session == "opaque-session" &&
                    event.toObject().value("cursor").toInteger() > after) {
                    auto relayed = event.toObject();
                    relayed.insert("occurredAt", "2026-11-12T09:00:02.000Z");
                    page.append(relayed);
                    next = event.toObject().value("cursor").toInteger();
                }
            return std::pair{200, QJsonObject{{"sessionId", session},
                                              {"sessionGeneration", 3},
                                              {"afterCursor", after},
                                              {"nextCursor", next},
                                              {"events", page}}};
        }
        return std::nullopt;
    }

    // As createSessionCommand / createCommand: a repeated idempotency key with the same body
    // answers the stored command; a different body is idempotency_conflict.
    std::optional<std::pair<int, QJsonObject>> accept(const QJsonObject& body,
                                                      const QByteArray& bytes, bool worktree) {
        const auto id = body.value("id").toString();
        ++submissions[id];
        if (refusedKinds.contains(body.value("kind").toString()))
            return std::pair{400, QJsonObject{}};
        if (unavailableKinds.contains(body.value("kind").toString()))
            return std::pair{unavailableKinds.value(body.value("kind").toString()), QJsonObject{}};
        if (worktree && ungranted.contains(body.value("worktreeId").toString()))
            return std::pair{403, QJsonObject{{"code", "session_create_forbidden"},
                                              {"message", "The worktree is not granted."}}};
        if (commands.contains(id)) {
            if (firstBodies.value(id) != bytes && ++conflicts)
                return std::pair{409, QJsonObject{{"code", "idempotency_conflict"},
                                                  {"message", "Idempotency conflict."}}};
        } else {
            auto dto = body;
            if (worktree) {
                dto.insert("sessionId", "");
                dto.insert("sessionGeneration", 0);
                dto.insert("capability", "session.create");
            }
            dto.insert("status", "queued");
            dto.insert("attempt", 1);
            dto.insert("claimedBy", QJsonValue::Null);
            dto.insert("leaseUntil", QJsonValue::Null);
            dto.insert("expiresAt", "2026-11-12T09:15:00.000Z");
            dto.insert("createdAt", "2026-11-12T09:00:00.000Z");
            dto.insert("completedAt", QJsonValue::Null);
            dto.insert("resultCode", QJsonValue::Null);
            dto.insert("encryptedResult", QJsonValue::Null);
            commands.insert(id, dto);
            firstBodies.insert(id, bytes);
        }
        if (dropSubmitAnswers > 0) {
            --dropSubmitAnswers;
            network.nextFailure = QNetworkReply::RemoteHostClosedError;
            return std::pair{0, QJsonObject{}};
        }
        return std::pair{202, commands.value(id)};
    }

    std::optional<std::pair<int, QJsonObject>> read(const QString& id) {
        ++commandReads;
        ++readsOfCommand[id];
        if (!commands.contains(id))
            return std::pair{404, QJsonObject{{"code", "command_not_found"}, {"message", ""}}};
        auto dto = commands.value(id);
        if (dto.value("status") == "queued" && !hostPaused) {
            execute(dto);
            commands.insert(id, dto);
        }
        return std::pair{200, dto};
    }

    // The host: decrypts with the client's pinned key, executes once, signs the result.
    void execute(QJsonObject& dto) {
        const auto id = dto.value("id").toString();
        const auto kind = dto.value("kind").toString();
        const auto worktree = control_v2::worktreeScoped(kind);
        QString error;
        const auto content = dto.value("content").toObject();
        const auto plaintext =
            worktree
                ? fixture.host.decryptSealedAs(content, ControlV2Client::commandAssociatedData(dto),
                                               clientSigning, id, "session-create:" + id, &error)
                : fixture.host.decrypt(content, ControlV2Client::commandAssociatedData(dto),
                                       clientSigning, dto.value("sessionId").toString(),
                                       dto.value("sessionGeneration").toInteger(), &error);
        if (!error.isEmpty())
            qFatal("Synthetic host rejected a command: %s", qPrintable(error));
        ++executions[id];
        plaintexts.insert(id, QJsonDocument::fromJson(plaintext).object());
        QJsonObject result = results ? results(kind, plaintexts.value(id)) : QJsonObject{};
        if (!result.isEmpty()) {
        } else if (kind == "worktree.configuration.read") {
            const auto worktreeId = dto.value("worktreeId").toString();
            QString label;
            for (const auto& value : std::as_const(worktrees))
                if (value.toObject().value("worktreeId") == worktreeId)
                    label = value.toObject().value("worktreeLabel").toString();
            result = {
                {"configuration", QJsonObject{}},
                {"worktree", QJsonObject{{"repositoryId", "repo-1"},
                                         {"worktreeId", worktreeId},
                                         {"repositoryLabel", "ketplus"},
                                         {"worktreeLabel", hostLabels.value(worktreeId, label)},
                                         {"catalogRevision", 0},
                                         {"lifecycleRevision", 0}}}};
        } else if (kind == "session.create") {
            result = {{"session_id", "created-session"}, {"generation", 1}, {"turn_sequence", 1}};
        } else if (kind == "session.start_turn") {
            result = {{"turn_sequence", 2}};
            events.append(fixture.event(10));
            events.append(fixture.event(11));
        } else if (kind == "session.pending_approvals") {
            result = {
                {"approvals",
                 QJsonArray{QJsonObject{{"approval_id", "approval-1"}, {"kind", "command"}},
                            QJsonObject{{"approval_id", "question-1"}, {"kind", "question"}}}}};
        } else {
            result = {{"accepted", kind}};
        }
        const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
        dto.insert("status", "completed");
        dto.insert("resultCode", "ok");
        dto.insert("completedAt", "2026-11-12T09:00:01.000Z");
        if (worktree) {
            const auto aad = CloudCrypto::canonical(
                {"ketplus", 2, "command-result", id, "workspace", "linux-host", "", 0, 1,
                 "completed", "ok", "session-create:" + id, 1, "linux-host", 1});
            dto.insert("encryptedResult",
                       fixture.host.encryptSealedAs(bytes, aad, clientAgreement, id,
                                                    "session-create:" + id, "remote-client", 1,
                                                    "linux-host", 1));
        } else {
            const auto session = dto.value("sessionId").toString();
            const auto generation = dto.value("sessionGeneration").toInteger();
            const auto keyId = session + ":" + QString::number(generation) + ":1";
            const auto aad = CloudCrypto::canonical(
                {"ketplus", 2, "command-result", id, "workspace", "linux-host", session, generation,
                 1, "completed", "ok", keyId, 1, "linux-host", 1});
            dto.insert("encryptedResult",
                       fixture.host.encrypt(bytes, aad, session, generation, 1, "linux-host", 1));
        }
    }
};

RelayMachineBackend::Options fastOptions(const QString& outbox) {
    RelayMachineBackend::Options options;
    options.outboxFile = outbox;
    options.eventPollMilliseconds = 20;
    options.commandPollMilliseconds = 10;
    options.initialBackoffMilliseconds = 20;
    options.maximumBackoffMilliseconds = 40;
    options.recordRetryMilliseconds = 20;
    return options;
}
} // namespace
