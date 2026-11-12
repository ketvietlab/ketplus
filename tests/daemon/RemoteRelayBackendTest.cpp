#include "RemoteControlFixture.h"
#include "control/ControlV2Content.h"
#include "control/RelayMachineBackend.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrlQuery>

// ADR 0119 Relay backend against a loopback relay whose replies are shaped like the deployed
// relay's DTOs (its session, worktree and command objects), with a synthetic host
// that decrypts each command, executes it once and signs its result.
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
    // Worktree label the host signs, when it differs from the relay's index.
    QHash<QString, QString> hostLabels;
    QJsonArray worktrees;
    QJsonArray events;
    int commandReads{0};

    SyntheticRelay(FixtureTransport& transport, EncryptedFixture& host, ControlV2Client& client)
        : network(transport), fixture(host) {
        const auto keys = client.identity().value("keys").toObject();
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
        // As the relay's sessionDto.
        network.replies.insert(
            prefix + "/v2/mobile/sessions",
            QJsonArray{
                QJsonObject{{"id", "opaque-session"},
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
                            {"encryptedMetadata",
                             sealedMetadata(fixture.host, "opaque-session", 3, 9, metadata)}}});
        worktrees = {worktreeDto("wt-a", "ketplus"), worktreeDto("wt-empty", "empty")};
        network.handler = [this](const QNetworkRequest& request, const QByteArray& body) {
            return handle(request, body);
        };
    }

    std::optional<std::pair<int, QJsonObject>> handle(const QNetworkRequest& request,
                                                      const QByteArray& bytes) {
        const auto path = request.url().path().mid(prefix.size());
        const auto method = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
        if (offline) {
            network.nextFailure = QNetworkReply::ConnectionRefusedError;
            return std::pair{0, QJsonObject{}};
        }
        if (path == "/v2/mobile/worktrees") {
            network.replies.insert(prefix + path, worktrees);
            return std::nullopt;
        }
        const auto body = QJsonDocument::fromJson(bytes).object();
        if (method == "POST" && path == "/v2/mobile/workspaces/workspace/session-creates")
            return accept(body, bytes, true);
        if (method == "POST" && path == "/v2/mobile/sessions/opaque-session/commands")
            return accept(body, bytes, false);
        if (path.startsWith("/v2/mobile/commands/"))
            return read(path.mid(QStringLiteral("/v2/mobile/commands/").size()));
        if (path == "/v2/mobile/sessions/opaque-session/events") {
            const auto after = QUrlQuery(request.url()).queryItemValue("afterCursor").toLongLong();
            QJsonArray page;
            qint64 next = after;
            for (const auto& event : std::as_const(events))
                if (event.toObject().value("cursor").toInteger() > after) {
                    page.append(event);
                    next = event.toObject().value("cursor").toInteger();
                }
            return std::pair{200, QJsonObject{{"sessionId", "opaque-session"},
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
        QJsonObject result;
        if (kind == "worktree.configuration.read") {
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
            const auto aad = CloudCrypto::canonical(
                {"ketplus", 2, "command-result", id, "workspace", "linux-host", "opaque-session", 3,
                 1, "completed", "ok", "opaque-session:3:1", 1, "linux-host", 1});
            dto.insert("encryptedResult",
                       fixture.host.encrypt(bytes, aad, "opaque-session", 3, 1, "linux-host", 1));
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
    return options;
}

struct Answer {
    bool done{false};
    QJsonObject result;
    QString error;
    MachineBackend::Callback callback() {
        done = false;
        return [this](const QJsonObject& value, const QString& failure) {
            result = value;
            error = failure;
            done = true;
        };
    }
};
} // namespace

class RemoteRelayBackendTest final : public QObject {
    Q_OBJECT

    static void connectAndWait(RelayMachineBackend& backend) {
        backend.connectToRelay();
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Connected);
    }
    static QJsonObject worktreeNamed(const QJsonObject& repositories, const QString& id) {
        for (const auto& group : repositories.value("repositories").toArray())
            for (const auto& item : group.toObject().value("worktrees").toArray())
                if (item.toObject().value("worktreeId") == id)
                    return item.toObject();
        return {};
    }
    static QString rejection(const QJsonObject& listing, const QString& id) {
        for (const auto& row : listing.value("rejected").toArray())
            if (row.toObject().value("id") == id)
                return row.toObject().value("reason").toString();
        return {};
    }

  private slots:
    void init() { r03::resetCredentials(); }

    void aFullTurnWithAnApprovalAndAQuestionAppliesHostSignedResults() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        QCOMPARE(backend.route(), MachineBackend::Route::Relay);
        QCOMPARE(backend.machineId(), CloudCrypto::keyFingerprint(fixture.hostKeys));
        Answer answer;
        // A remote route fails a request made before Connected.
        backend.listConversations({}, answer.callback());
        QVERIFY(answer.done);
        QVERIFY(!answer.error.isEmpty());
        connectAndWait(backend);
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        const auto sessions = answer.result.value("sessions").toArray();
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.first().toObject().value("title").toString(), "Sealed title");

        QSignalSpy events(&backend, &MachineBackend::eventReceived);
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        backend.startTurn("opaque-session", 3, {{"prompt", "private fixture prompt"}},
                          answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QCOMPARE(answer.result.value("turn_sequence").toInt(), 2);
        // The relay only ever saw ciphertext.
        for (const auto& body : std::as_const(network.bodies))
            QVERIFY(!body.contains("private fixture prompt"));
        QTRY_COMPARE(events.size(), 2);
        QCOMPARE(events.at(1).at(0).toJsonObject().value("cursor").toInt(), 11);

        backend.pendingApprovals("opaque-session", 3, {}, answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QCOMPARE(answer.result.value("approvals").toArray().size(), 2);
        backend.resolveApproval("opaque-session", 3, "approval-1", {{"decision", "approve"}},
                                answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        backend.answerQuestion("opaque-session", 3, "question-1", {{"answers", QJsonArray{"yes"}}},
                               answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QCOMPARE(completed.size(), 4);

        QSet<QString> kinds;
        for (auto it = relay.plaintexts.cbegin(); it != relay.plaintexts.cend(); ++it) {
            const auto kind = relay.commands.value(it.key()).value("kind").toString();
            // The listing also verified wt-empty, which has no conversation.
            if (kind == "worktree.configuration.read")
                continue;
            kinds.insert(kind);
            if (kind == "approval.resolve")
                QCOMPARE(it.value().value("approval_id").toString(), "approval-1");
            if (kind == "question.answer")
                QCOMPARE(it.value().value("approval_id").toString(), "question-1");
            if (kind == "session.start_turn")
                QCOMPARE(it.value().value("prompt").toString(), "private fixture prompt");
            QCOMPARE(relay.executions.value(it.key()), 1);
        }
        QCOMPARE(kinds, (QSet<QString>{"session.start_turn", "session.pending_approvals",
                                       "approval.resolve", "question.answer"}));
        QVERIFY(backend.pendingCommands().isEmpty());
    }

    void aSessionIsCreatedInAnEmptyWorktreeTheHostVerified() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RemoteControlTestAccess::restorePoll(client, 10);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        Answer answer;
        backend.listRepositories(answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        const auto empty = worktreeNamed(
            {{"repositories", QJsonArray{answer.result.value("repositories").toArray().first()}}},
            "wt-empty");
        QCOMPARE(empty.value("verified").toString(), "host_read");
        QCOMPARE(empty.value("label").toString(), "empty");
        QCOMPARE(worktreeNamed(answer.result, "wt-a").value("verified").toString(), "conversation");

        // A worktree the relay never listed, and no prompt, never leave this device.
        const auto before = network.paths.size();
        backend.createSession(
            {{"repository_id", "repo-1"}, {"worktree_id", "wt-unknown"}, {"prompt", "hello"}},
            answer.callback());
        QVERIFY(answer.done);
        QVERIFY(!answer.error.isEmpty());
        backend.createSession({{"repository_id", "repo-1"}, {"worktree_id", "wt-empty"}},
                              answer.callback());
        QVERIFY(answer.done);
        QVERIFY(!answer.error.isEmpty());
        QCOMPARE(network.paths.size(), before);

        backend.createSession({{"repository_id", "repo-1"},
                               {"worktree_id", "wt-empty"},
                               {"prompt", "private first prompt"},
                               {"options", QJsonObject{{"model", "fixture"}}}},
                              answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QCOMPARE(answer.result.value("session_id").toString(), "created-session");
        QString created;
        for (auto it = relay.commands.cbegin(); it != relay.commands.cend(); ++it)
            if (it.value().value("kind") == "session.create")
                created = it.key();
        QVERIFY(!created.isEmpty());
        const auto dto = relay.commands.value(created);
        QCOMPARE(dto.value("worktreeId").toString(), "wt-empty");
        QCOMPARE(dto.value("ownerSubjectId").toString(), "fixture-owner");
        QCOMPARE(relay.plaintexts.value(created).value("prompt").toString(),
                 "private first prompt");
        QCOMPARE(relay.executions.value(created), 1);
        for (const auto& body : std::as_const(network.bodies))
            QVERIFY(!body.contains("private first prompt"));
    }

    void indexWorktreesAreShownOnlyWhenTheHostSignsTheSameScope() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RemoteControlTestAccess::restorePoll(client, 10);
        relay.worktrees.append(worktreeDto("wt-moved", "renamed by relay"));
        relay.hostLabels.insert("wt-moved", "original");
        relay.worktrees.append(worktreeDto("wt-locked", "locked"));
        relay.ungranted.insert("wt-locked");
        auto stranger = worktreeDto("wt-stranger", "stranger");
        stranger.insert("machineId", "stranger-host");
        relay.worktrees.append(stranger);
        auto options = fastOptions(dir.filePath("outbox.json"));
        RelayMachineBackend backend(&client, "linux-host", options);
        connectAndWait(backend);
        Answer answer;
        backend.listWorktrees({}, 0, {{"repository_id", "repo-1"}}, answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QStringList shown;
        for (const auto& item : answer.result.value("worktrees").toArray())
            shown.append(item.toObject().value("id").toString());
        QCOMPARE(shown, (QStringList{"wt-a", "wt-empty"}));
        QCOMPARE(answer.result.value("total").toInt(), 2);

        backend.listRepositories(answer.callback());
        QTRY_VERIFY(answer.done);
        QCOMPARE(rejection(answer.result, "wt-moved"), "verification_failed");
        QCOMPARE(rejection(answer.result, "wt-locked"), "worktree_not_granted");
        // Another machine's entries belong to that machine's backend.
        QVERIFY(rejection(answer.result, "wt-stranger").isEmpty());
        // A rejected worktree cannot receive a session.
        backend.createSession(
            {{"repository_id", "repo-1"}, {"worktree_id", "wt-moved"}, {"prompt", "hi"}},
            answer.callback());
        QVERIFY(answer.done);
        QVERIFY(!answer.error.isEmpty());

        // Beyond the bound, an index entry is reported unsigned without asking the host.
        options.verifyWorktrees = 0;
        RelayMachineBackend bounded(&client, "linux-host", options);
        connectAndWait(bounded);
        const auto reads = relay.commandReads;
        bounded.listRepositories(answer.callback());
        QTRY_VERIFY(answer.done);
        QCOMPARE(rejection(answer.result, "wt-empty"), "unsigned_index_entry");
        QCOMPARE(relay.commandReads, reads);
    }

    void theOutboxRetriesTheSameCiphertextAfterARestartAndAppliesItOnce() {
        QTemporaryDir dir;
        const auto outbox = dir.filePath("machines/linux-host/outbox.json");
        FixtureTransport network;
        auto first = std::make_unique<ControlV2Client>(QUrl{origin});
        EncryptedFixture fixture(*first, network);
        RemoteControlTestAccess::persist(*first);
        SyntheticRelay relay(network, fixture, *first);
        QString id;
        {
            RelayMachineBackend backend(first.get(), "linux-host", fastOptions(outbox));
            connectAndWait(backend);
            Answer answer;
            backend.listConversations({}, answer.callback());
            QTRY_VERIFY(answer.done);
            // The relay stores the command, then the link drops before its answer arrives.
            relay.dropSubmitAnswers = 1;
            relay.hostPaused = true;
            backend.startTurn("opaque-session", 3, {{"prompt", "private retried prompt"}},
                              answer.callback());
            const auto turn = [&relay] {
                for (auto it = relay.commands.cbegin(); it != relay.commands.cend(); ++it)
                    if (it.value().value("kind") == "session.start_turn")
                        return it.key();
                return QString();
            };
            QTRY_VERIFY(!turn().isEmpty());
            id = turn();
            // The backend may already resubmit after its own reconnect; the host stays away.
            QCOMPARE(backend.pendingCommands(), QStringList{id});
            relay.offline = true;
            QTRY_VERIFY(backend.connectionState() != ConnectionState::Connected);
            QCOMPARE(backend.pendingCommands(), QStringList{id});
        }
        // Only ciphertext is on disk.
        QFile file(outbox);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto stored = file.readAll();
        QVERIFY(stored.contains(id.toUtf8()));
        QVERIFY(!stored.contains("private retried prompt"));
        QCOMPARE(file.permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions{});
        first.reset();

        // The restarted Desktop: a new client from the stored credential, a new backend.
        ControlV2Client second{QUrl{origin}};
        RemoteControlTestAccess::transport(second, network);
        relay.offline = false;
        relay.hostPaused = false;
        RelayMachineBackend backend(&second, "linux-host", fastOptions(outbox));
        QCOMPARE(backend.pendingCommands(), QStringList{id});
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        connectAndWait(backend);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed.first().at(0).toString(), id);
        QCOMPARE(completed.first().at(1).toJsonObject().value("turn_sequence").toInt(), 2);
        QVERIFY(completed.first().at(2).toString().isEmpty());
        // The same bytes every time: the relay answered the stored command, the host ran it once.
        const auto submitted = relay.submissions.value(id);
        QVERIFY(submitted >= 2);
        QCOMPARE(relay.conflicts, 0);
        QCOMPARE(relay.commands.value(id).value("status").toString(), "completed");
        QCOMPARE(relay.executions.value(id), 1);
        QVERIFY(backend.pendingCommands().isEmpty());

        // Another restart reads the result again but never re-applies it.
        RelayMachineBackend third(&second, "linux-host", fastOptions(outbox));
        QSignalSpy again(&third, &RelayMachineBackend::commandCompleted);
        connectAndWait(third);
        QTest::qWait(100);
        QCOMPARE(again.size(), 0);
        QCOMPARE(relay.submissions.value(id), submitted);
    }

    void goingOfflineReconnectsAndARevokedCredentialFencesInFlightResults() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        QSignalSpy states(&backend, &MachineBackend::connectionStateChanged);
        QCOMPARE(backend.connectionState(), ConnectionState::Connecting);
        relay.offline = true;
        backend.connectToRelay();
        QTest::qWait(100);
        QCOMPARE(backend.connectionState(), ConnectionState::Connecting);
        relay.offline = false;
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Connected);

        Answer answer;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        relay.offline = true;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY(!answer.error.isEmpty());
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Reconnecting);
        relay.offline = false;
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Connected);
        QList<ConnectionState> seen;
        for (const auto& change : std::as_const(states))
            seen.append(change.at(0).value<ConnectionState>());
        QVERIFY(seen.contains(ConnectionState::Reconnecting));
        QCOMPARE(seen.last(), ConnectionState::Connected);

        backend.disconnectFromRelay();
        QCOMPARE(backend.connectionState(), ConnectionState::Offline);
        connectAndWait(backend);

        // A command in flight when the credential is revoked never reaches the UI.
        relay.hostPaused = true;
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        backend.stopTurn("opaque-session", 3, answer.callback());
        QTRY_VERIFY(relay.commandReads > 0);
        RemoteControlTestAccess::expireCredential(client);
        const auto refresh = prefix + "/v2/auth/refresh";
        network.replies.insert(refresh, QJsonObject{{"code", "user_session_revoked"}});
        network.statuses.insert(refresh, 401);
        relay.hostPaused = false;
        QTRY_VERIFY(answer.done);
        QVERIFY(!answer.error.isEmpty());
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Offline);
        QTest::qWait(100);
        QCOMPARE(completed.size(), 0);
        QCOMPARE(backend.connectionState(), ConnectionState::Offline);
        // No retry until the user signs in again.
        backend.connectToRelay();
        QCOMPARE(backend.connectionState(), ConnectionState::Offline);
    }
};

QTEST_GUILESS_MAIN(RemoteRelayBackendTest)
#include "RemoteRelayBackendTest.moc"
