#include "MachineAttachmentUpload.h"
#include "SyntheticRelay.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>

// ADR 0119 Relay backend against the loopback relay of SyntheticRelay.h.
namespace {
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
    // A repository listing once the worktree verifications it queued have landed.
    static void settledListing(RelayMachineBackend& backend, QJsonObject* listing) {
        QSignalSpy verified(&backend, &MachineBackend::catalogChanged);
        Answer answer;
        backend.listRepositories(answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        const auto pending = answer.result.value("pending").toArray().size();
        if (pending > 0) {
            QTRY_VERIFY(verified.size() >= pending);
            backend.listRepositories(answer.callback());
            QTRY_VERIFY(answer.done);
            QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        }
        QVERIFY(answer.result.value("pending").toArray().isEmpty());
        *listing = answer.result;
    }
    static int readsOf(const SyntheticRelay& relay, const QString& kind) {
        int count = 0;
        for (const auto& command : relay.commands)
            count += command.value("kind").toString() == kind;
        return count;
    }
    static QString commandOf(const SyntheticRelay& relay, const QString& kind) {
        for (auto it = relay.commands.cbegin(); it != relay.commands.cend(); ++it)
            if (it.value().value("kind") == kind)
                return it.key();
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

    void preparationIsCapabilityGatedAndUsesAPrivateHostSignedScope_data() {
        QTest::addColumn<bool>("supported");
        QTest::addColumn<bool>("cancel");
        QTest::addColumn<bool>("refuse");
        QTest::newRow("old-relay") << false << false << false;
        QTest::newRow("first-turn") << true << false << false;
        QTest::newRow("cancel") << true << true << false;
        QTest::newRow("prepare-refused") << true << false << true;
    }
    void preparationIsCapabilityGatedAndUsesAPrivateHostSignedScope() {
        QFETCH(bool, supported);
        QFETCH(bool, cancel);
        QFETCH(bool, refuse);
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        if (!supported)
            relay.machinesStatus = 404;
        if (supported)
            network.replies.insert(
                prefix + "/v2/machines",
                QJsonArray{QJsonObject{{"id", "linux-host"},
                                       {"capabilities", QJsonArray{"session.prepare/1"}}}});
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        QCOMPARE(backend.supports(MachineBackend::Feature::SessionPreparation), supported);
        QVERIFY(backend.supports(MachineBackend::Feature::Attachments));
        QJsonObject listing;
        settledListing(backend, &listing);
        relay.results = [&](const QString& kind, const QJsonObject& payload) -> QJsonObject {
            if (kind == "session.prepare") {
                if (refuse)
                    return {{"ok", false}, {"error_message", "Preparation refused"}};
                if (payload.value("client_request_id") != "draft-1" || payload.contains("prompt"))
                    return {{"ok", false}, {"error_message", "Invalid preparation"}};
                const auto keys = client.identity().value("keys").toObject();
                const auto wrapped = fixture.host.wrapSessionKey(
                    "prepared-private", 1, 1,
                    {{"id", "remote-client"}, {"keyVersion", 1}, {"keys", keys}}, "linux-host", 1);
                network.replies.insert(prefix + "/v2/mobile/sessions/prepared-private/keys",
                                       QJsonArray{wrapped});
                return {{"ok", true},
                        {"session_id", "prepared-private"},
                        {"session_generation", 1},
                        {"expires_at_ms", QDateTime::currentMSecsSinceEpoch() + 60000}};
            }
            return {{"ok", true}};
        };
        Answer prepared;
        backend.prepareSession({{"repository_id", "repo-1"},
                                {"worktree_id", "wt-a"},
                                {"client_request_id", "draft-1"}},
                               prepared.callback());
        QTRY_VERIFY(prepared.done);
        if (!supported || refuse) {
            QVERIFY(!prepared.error.isEmpty());
            QCOMPARE(readsOf(relay, "session.prepare"), supported ? 1 : 0);
            return;
        }
        QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
        QCOMPARE(prepared.result.value("session_id"), "prepared-private");
        QVERIFY(!relay.eventPolls.contains("prepared-private"));
        Answer sessions;
        backend.listConversations({}, sessions.callback());
        QTRY_VERIFY(sessions.done);
        QVERIFY(!QJsonDocument(sessions.result).toJson().contains("prepared-private"));
        Answer stage;
        backend.attachmentCommand(MachineBackend::AttachmentOperation::Begin, "prepared-private", 1,
                                  {{"attachment_id", "file-1"}}, stage.callback());
        QTRY_VERIFY(stage.done);
        QVERIFY2(stage.error.isEmpty(), qPrintable(stage.error));
        QVERIFY(!relay.eventPolls.contains("prepared-private"));
        Answer finished;
        if (cancel)
            backend.cancelPreparedSession("prepared-private", 1, finished.callback());
        else
            backend.startTurn("prepared-private", 1, {{"prompt", "read"}}, finished.callback());
        QTRY_VERIFY(finished.done);
        QVERIFY2(finished.error.isEmpty(), qPrintable(finished.error));
        if (cancel) {
            backend.attachmentCommand(MachineBackend::AttachmentOperation::Begin,
                                      "prepared-private", 1, {}, stage.callback());
            QTRY_VERIFY(stage.done);
            QVERIFY(!stage.error.isEmpty());
        }
        QCOMPARE(readsOf(relay, "session.create"), 0);
        for (const auto& body : relay.firstBodies)
            QVERIFY(!body.contains("draft-1"));
    }

    void attachmentUploadResumesQueuedStepsAndRejectsFailedChunks() {
        const QList<QPair<QString, bool>> scenarios{
            {"text/plain", false}, {"image/png", false}, {"text/plain", true}};
        for (const auto& scenario : scenarios) {
            const QString mediaType = scenario.first;
            const bool rejectChunk = scenario.second;
            r03::resetCredentials();
            QTemporaryDir dir;
            FixtureTransport network;
            ControlV2Client client(QUrl{origin});
            EncryptedFixture fixture(client, network);
            SyntheticRelay relay(network, fixture, client);
            RelayMachineBackend backend(&client, "linux-host",
                                        fastOptions(dir.filePath("outbox.json")));
            connectAndWait(backend);
            QVERIFY(backend.supports(MachineBackend::Feature::Attachments));
            Answer listing;
            backend.listConversations({}, listing.callback());
            QTRY_VERIFY(listing.done);
            QVERIFY2(listing.error.isEmpty(), qPrintable(listing.error));
            QByteArray received;
            QString digest;
            relay.results = [&](const QString& kind, const QJsonObject& payload) -> QJsonObject {
                if (kind == "attachment.stage.begin") {
                    if (payload.value("media_type").toString() != mediaType)
                        return {{"ok", false}, {"error_message", "Media type changed"}};
                    digest = payload.value("digest_sha256").toString();
                    return {{"revision", 1}};
                }
                if (kind == "attachment.stage.chunk") {
                    if (rejectChunk)
                        return {{"ok", false}, {"error_message", "Chunk refused"}};
                    received += QByteArray::fromBase64(payload.value("data").toString().toLatin1(),
                                                       QByteArray::Base64UrlEncoding);
                    return {{"accepted", true}};
                }
                if (kind == "attachment.stage.commit") {
                    if (QString::fromLatin1(
                            QCryptographicHash::hash(received, QCryptographicHash::Sha256)
                                .toHex()) != digest)
                        return {{"ok", false}, {"error_message", "Digest mismatch"}};
                    return {{"status", "ready"}};
                }
                return {};
            };
            relay.hostPaused = true;
            ketplus::remote::MachineAttachmentUpload upload(&backend, "opaque-session", 3);
            QSignalSpy finished(&upload, &ketplus::remote::MachineAttachmentUpload::finished);
            const QByteArray bytes =
                mediaType == "text/plain"
                    ? QByteArray(40 * 1024, 't')
                    : QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQ"
                                             "VR42mP8/x8AAwMCAO+aWQ0AAAAASUVORK5CYII=");
            upload.start(mediaType == "text/plain" ? "synthetic-note.txt" : "synthetic-note.png",
                         mediaType, bytes);
            QTRY_COMPARE(readsOf(relay, "attachment.stage.begin"), 1);
            QCOMPARE(finished.size(), 0);
            QVERIFY(received.isEmpty());
            // A link fence returns the durable command as queued; completion must
            // still arrive through commandCompleted after reconnect, not advance early.
            backend.disconnectFromRelay();
            QCOMPARE(finished.size(), 0);
            QVERIFY(received.isEmpty());
            relay.hostPaused = false;
            connectAndWait(backend);
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
            QVERIFY2(upload.ready() == !rejectChunk, qPrintable(finished[0][1].toString()));
            if (rejectChunk)
                QVERIFY(!finished[0][1].toString().isEmpty());
            else
                QCOMPARE(received, bytes);
            for (const auto& body : std::as_const(network.bodies)) {
                QVERIFY(!body.contains("synthetic-note"));
                QVERIFY(!body.contains(QByteArray(128, 't')));
            }
        }
    }

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
        // A request made while Connecting waits for Connected (ADR 0119).
        backend.listConversations({}, answer.callback());
        QVERIFY(!answer.done);
        connectAndWait(backend);
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
        // The same keys as the local daemon's session.event (LocalIpcServer::broadcastEvent).
        const auto live = events.at(1).at(0).toJsonObject();
        const auto liveKeys = live.keys();
        QCOMPARE(
            QSet<QString>(liveKeys.cbegin(), liveKeys.cend()),
            (QSet<QString>{"protocol_version", "message_id", "message_type", "session_id",
                           "session_generation", "cursor", "turn_sequence", "sent_at", "payload"}));
        QCOMPARE(live.value("message_type").toString(), "session.event");
        QCOMPARE(live.value("session_id").toString(), "opaque-session");
        QCOMPARE(live.value("session_generation").toInt(), 3);
        QCOMPARE(live.value("turn_sequence").toInt(), 1);
        QCOMPARE(live.value("sent_at").toString(), "2026-11-12T09:00:02.000Z");
        const auto payload = live.value("payload").toObject();
        const auto payloadKeys = payload.keys();
        QCOMPARE(QSet<QString>(payloadKeys.cbegin(), payloadKeys.cend()),
                 (QSet<QString>{"event_type", "data"}));
        QCOMPARE(payload.value("event_type").toString(), "assistant.delta");
        QCOMPARE(payload.value("data").toObject().value("delta").toString(),
                 "private fixture text");

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
        // pendingApprovals is a read: it has a result but no outbox completion.
        QCOMPARE(completed.size(), 3);

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

    // T-18: rename and fork are Control v2 commands; archive is not offered on this route, so it
    // is never sent and says why.
    void renameAndForkAreCommandsAndArchiveIsNotOffered() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        using Feature = MachineBackend::Feature;
        QVERIFY(!backend.supports(Feature::Rename)); // not Connected yet
        connectAndWait(backend);
        Answer listed;
        backend.listConversations({}, listed.callback());
        QTRY_VERIFY(listed.done);
        QVERIFY(backend.supports(Feature::Rename));
        QVERIFY(backend.supports(Feature::Fork));
        QVERIFY(!backend.supports(Feature::Archive));
        QVERIFY(backend.supports(Feature::Attachments));
        QCOMPARE(backend.unsupportedReason(Feature::Archive),
                 QStringLiteral("Not available through the Relay yet."));

        Answer renamed;
        backend.renameConversation("opaque-session", 3, "Release notes", renamed.callback());
        QTRY_VERIFY(renamed.done);
        QVERIFY2(renamed.error.isEmpty(), qPrintable(renamed.error));
        Answer forked;
        backend.forkConversation("opaque-session", 3, "fork-1", forked.callback());
        QTRY_VERIFY(forked.done);
        QVERIFY2(forked.error.isEmpty(), qPrintable(forked.error));
        Answer archived;
        backend.setConversationArchived("opaque-session", 3, true, archived.callback());
        QVERIFY(archived.done);
        QCOMPARE(archived.error, QStringLiteral("Not available through the Relay yet."));

        const auto rename = commandOf(relay, "session.rename");
        QVERIFY(!rename.isEmpty());
        QCOMPARE(relay.plaintexts.value(rename).value("title").toString(), "Release notes");
        const auto fork = commandOf(relay, "session.fork");
        QVERIFY(!fork.isEmpty());
        QCOMPARE(relay.plaintexts.value(fork).value("fork_session_id").toString(), "fork-1");
        QVERIFY(commandOf(relay, "session.set_archived").isEmpty());
        for (const auto& body : std::as_const(network.bodies))
            QVERIFY(!body.contains("Release notes"));
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
        QJsonObject listing;
        settledListing(backend, &listing);
        answer.result = listing;
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

        backend.createSession(
            {{"repository_id", "repo-1"},
             {"worktree_id", "wt-empty"},
             {"prompt", "private first prompt"},
             {"options", QJsonObject{{"model", "fixture"}, {"provider", "copilot"}}}},
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
        QCOMPARE(relay.plaintexts.value(created)
                     .value("options")
                     .toObject()
                     .value("provider")
                     .toString(),
                 QStringLiteral("copilot"));
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
        QJsonObject listing;
        settledListing(backend, &listing);
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

        // Beyond the bound, a new index entry is reported unsigned without asking the host.
        relay.worktrees.append(worktreeDto("wt-new", "new"));
        options.verifyWorktrees = 0;
        RelayMachineBackend bounded(&client, "linux-host", options);
        connectAndWait(bounded);
        const auto reads = relay.commandReads;
        bounded.listRepositories(answer.callback());
        QTRY_VERIFY(answer.done);
        QCOMPARE(rejection(answer.result, "wt-new"), "unsigned_index_entry");
        QVERIFY(answer.result.value("pending").toArray().isEmpty());
        QTest::qWait(50);
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

    // Desktop has one backend per Relay host for all its windows (ADR 0119, T-20), so it is the
    // host's only outbox writer: commands two windows queue while the host is away are all kept
    // across a restart and each is applied once.
    void commandsTwoWindowsQueueOnTheSharedBackendSurviveARestart() {
        QTemporaryDir dir;
        const auto outbox = dir.filePath("machines/linux-host/outbox.json");
        FixtureTransport network;
        auto first = std::make_unique<ControlV2Client>(QUrl{origin});
        EncryptedFixture fixture(*first, network);
        RemoteControlTestAccess::persist(*first);
        SyntheticRelay relay(network, fixture, *first);
        QStringList ids;
        {
            RelayMachineBackend backend(first.get(), "linux-host", fastOptions(outbox));
            connectAndWait(backend);
            Answer listing;
            backend.listConversations({}, listing.callback());
            QTRY_VERIFY(listing.done);
            relay.hostPaused = true;
            // Both windows show the same conversation; each sends its own command.
            Answer fromFirstWindow;
            Answer fromSecondWindow;
            backend.startTurn("opaque-session", 3, {{"prompt", "first"}},
                              fromFirstWindow.callback());
            backend.stopTurn("opaque-session", 3, fromSecondWindow.callback());
            QTRY_COMPARE(backend.pendingCommands().size(), 2);
            ids = backend.pendingCommands();
            relay.offline = true;
            QTRY_VERIFY(backend.connectionState() != ConnectionState::Connected);
        }
        first.reset();

        ControlV2Client second{QUrl{origin}};
        RemoteControlTestAccess::transport(second, network);
        relay.offline = false;
        relay.hostPaused = false;
        RelayMachineBackend backend(&second, "linux-host", fastOptions(outbox));
        QCOMPARE(backend.pendingCommands(), ids);
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        connectAndWait(backend);
        QTRY_COMPARE(completed.size(), 2);
        QStringList done{completed.at(0).at(0).toString(), completed.at(1).at(0).toString()};
        done.sort();
        ids.sort();
        QCOMPARE(done, ids);
        for (const auto& id : std::as_const(ids))
            QCOMPARE(relay.executions.value(id), 1);
        QVERIFY(backend.pendingCommands().isEmpty());
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
        QTRY_VERIFY(!commandOf(relay, "session.interrupt").isEmpty());
        RemoteControlTestAccess::expireCredential(client);
        const auto refresh = prefix + "/v2/auth/refresh";
        network.replies.insert(refresh, QJsonObject{{"code", "user_session_revoked"}});
        network.statuses.insert(refresh, 401);
        relay.hostPaused = false;
        // The relay holds the command and may still run it: queued, not failed.
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QCOMPARE(answer.result.value("status").toString(), "queued");
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Offline);
        QTest::qWait(100);
        QCOMPARE(completed.size(), 0);
        QCOMPARE(backend.connectionState(), ConnectionState::Offline);
        // No retry until the user signs in again.
        backend.connectToRelay();
        QCOMPARE(backend.connectionState(), ConnectionState::Offline);
    }

    void worktreeVerificationIsLazyCachedAndBounded() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RemoteControlTestAccess::restorePoll(client, 10);
        for (int i = 0; i < 8; ++i)
            relay.worktrees.append(worktreeDto(QStringLiteral("wt-%1").arg(i), "more"));
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        // The host is away: a listing still answers at once and never waits for it.
        relay.hostPaused = true;
        QSignalSpy verified(&backend, &MachineBackend::catalogChanged);
        Answer answer;
        QElapsedTimer clock;
        clock.start();
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(answer.result.value("sessions").toArray().size(), 1);
        backend.listRepositories(answer.callback());
        QTRY_VERIFY(answer.done);
        QCOMPARE(answer.result.value("pending").toArray().size(), 9);
        QVERIFY(worktreeNamed(answer.result, "wt-empty").isEmpty());
        // At most four checks run at a time.
        QTest::qWait(200);
        QCOMPARE(readsOf(relay, "worktree.configuration.read"), 4);
        relay.hostPaused = false;
        QTRY_VERIFY_WITH_TIMEOUT(verified.size() >= 9, 10000);
        QCOMPARE(readsOf(relay, "worktree.configuration.read"), 9);
        // Each scope is verified once: later listings use the cached verdict.
        QJsonObject listing;
        settledListing(backend, &listing);
        settledListing(backend, &listing);
        QCOMPARE(worktreeNamed(listing, "wt-7").value("verified").toString(), "host_read");
        QCOMPARE(readsOf(relay, "worktree.configuration.read"), 9);
        // A changed label is a new scope, verified again.
        auto renamed = relay.worktrees.at(1).toObject();
        renamed.insert("worktreeLabel", "renamed");
        relay.hostLabels.insert("wt-empty", "renamed");
        relay.worktrees.replace(1, renamed);
        settledListing(backend, &listing);
        QCOMPARE(readsOf(relay, "worktree.configuration.read"), 10);
        QCOMPARE(worktreeNamed(listing, "wt-empty").value("label").toString(), "renamed");
    }

    void destroyingTheBackendMidRequestDropsItsCallbacks() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        Answer listing;
        Answer turn;
        {
            auto backend = std::make_unique<RelayMachineBackend>(
                &client, "linux-host", fastOptions(dir.filePath("outbox.json")));
            connectAndWait(*backend);
            backend->listConversations({}, listing.callback());
            QTRY_VERIFY(listing.done);
            relay.hostPaused = true;
            backend->listConversations({}, listing.callback());
            backend->startTurn("opaque-session", 3, {{"prompt", "hi"}}, turn.callback());
            backend.reset();
        }
        relay.hostPaused = false;
        QTest::qWait(200);
        // Nothing reached the destroyed backend or its callers.
        QVERIFY(!listing.done);
        QVERIFY(!turn.done);
        // The shared client keeps serving another backend.
        RelayMachineBackend again(&client, "linux-host", fastOptions(dir.filePath("other.json")));
        connectAndWait(again);
        again.listConversations({}, listing.callback());
        QTRY_VERIFY(listing.done);
        QVERIFY2(listing.error.isEmpty(), qPrintable(listing.error));
    }

    void liveEventsResumeAfterAFenceDuringAnEventsPoll() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        Answer answer;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        // A turn makes the backend follow the conversation's events.
        QSignalSpy before(&backend, &MachineBackend::eventReceived);
        backend.startTurn("opaque-session", 3, {{"prompt", "hi"}}, answer.callback());
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QTRY_COMPARE(before.size(), 2);
        // The link is fenced while an events request is in flight.
        bool fenced = false;
        relay.onEvents = [&] {
            if (fenced)
                return;
            fenced = true;
            QTimer::singleShot(0, &backend, [&backend] { backend.disconnectFromRelay(); });
        };
        QTRY_VERIFY(fenced);
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Offline);
        QTest::qWait(50);
        QSignalSpy events(&backend, &MachineBackend::eventReceived);
        connectAndWait(backend);
        relay.events.append(fixture.event(12));
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(events.first().at(0).toJsonObject().value("cursor").toInt(), 12);
    }

    void aReportedFailureIsNeverSentAndAQueuedCommandCompletesOnce() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        Answer answer;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);

        // Reconnecting: a request waits, and completes once the link is back.
        relay.offline = true;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Reconnecting);
        backend.listConversations({}, answer.callback());
        QTest::qWait(100);
        QVERIFY(!answer.done);
        relay.offline = false;
        QTRY_VERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));

        // A command held while Reconnecting that is then failed by going Offline was never
        // sent, and never will be.
        relay.offline = true;
        backend.listConversations({}, answer.callback());
        QTRY_COMPARE(backend.connectionState(), ConnectionState::Reconnecting);
        Answer held;
        backend.startTurn("opaque-session", 3, {{"prompt", "never sent"}}, held.callback());
        QTest::qWait(50);
        QVERIFY(!held.done);
        backend.disconnectFromRelay();
        QVERIFY(held.done);
        QVERIFY(!held.error.isEmpty());
        // Offline fails at once.
        backend.startTurn("opaque-session", 3, {{"prompt", "never sent"}}, held.callback());
        QVERIFY(held.done);
        QVERIFY(!held.error.isEmpty());
        relay.offline = false;
        connectAndWait(backend);
        QTest::qWait(100);
        QVERIFY(commandOf(relay, "session.start_turn").isEmpty());
        QVERIFY(backend.pendingCommands().isEmpty());

        // A command the relay holds is queued, not failed, and its outcome arrives once.
        relay.hostPaused = true;
        Answer queued;
        backend.startTurn("opaque-session", 3, {{"prompt", "queued"}}, queued.callback());
        QTRY_VERIFY(!commandOf(relay, "session.start_turn").isEmpty());
        const auto id = commandOf(relay, "session.start_turn");
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        backend.disconnectFromRelay();
        QVERIFY(queued.done);
        QVERIFY2(queued.error.isEmpty(), qPrintable(queued.error));
        QCOMPARE(queued.result.value("status").toString(), "queued");
        QCOMPARE(queued.result.value("command_id").toString(), id);
        relay.hostPaused = false;
        connectAndWait(backend);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed.first().at(0).toString(), id);
        QVERIFY(completed.first().at(2).toString().isEmpty());
        QCOMPARE(relay.executions.value(id), 1);
    }

    void aResultIsAppliedOnlyAfterItsCompletionIsRecorded() {
        QTemporaryDir dir;
        const auto outbox = dir.filePath("outbox.json");
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host", fastOptions(outbox));
        connectAndWait(backend);
        Answer answer;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        relay.hostPaused = true;
        backend.startTurn("opaque-session", 3, {{"prompt", "recorded"}}, answer.callback());
        QTRY_VERIFY(!commandOf(relay, "session.start_turn").isEmpty());
        const auto id = commandOf(relay, "session.start_turn");
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        // The outbox cannot be written: a directory stands where the file was.
        QVERIFY(QFile::remove(outbox));
        QVERIFY(QDir().mkpath(outbox + "/blocked"));
        // One warning however long the file stays unwritable.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("^Relay command outbox not updated"));
        QTest::failOnWarning(QRegularExpression("^Relay command outbox not updated"));
        relay.hostPaused = false;
        QTRY_COMPARE(relay.executions.value(id), 1);
        const auto readsBefore = relay.readsOfCommand.value(id);
        QTest::qWait(300);
        // Backed off (20, 40, 80, 160 ms ...): a few read-backs, not one per 20 ms.
        const auto readBacks = relay.readsOfCommand.value(id) - readsBefore;
        QVERIFY2(readBacks >= 1 && readBacks <= 6, qPrintable(QString::number(readBacks)));
        QCOMPARE(completed.size(), 0);
        QVERIFY(!answer.done);
        QCOMPARE(backend.pendingCommands(), QStringList{id});
        // Once it can be recorded it is applied, once, while the link stays up.
        QVERIFY(QDir(outbox).removeRecursively());
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(answer.done);
        QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
        QCOMPARE(answer.result.value("turn_sequence").toInt(), 2);
        QCOMPARE(backend.connectionState(), ConnectionState::Connected);
        QTest::qWait(100);
        QCOMPARE(completed.size(), 1);
        QCOMPARE(relay.executions.value(id), 1);
        QVERIFY(backend.pendingCommands().isEmpty());
    }

    void aReadIsNeverStoredInTheOutbox() {
        QTemporaryDir dir;
        const auto outbox = dir.filePath("outbox.json");
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        QString id;
        {
            RelayMachineBackend backend(&client, "linux-host", fastOptions(outbox));
            connectAndWait(backend);
            Answer answer;
            backend.listConversations({}, answer.callback());
            QTRY_VERIFY(answer.done);
            QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
            relay.hostPaused = true;
            backend.pendingApprovals("opaque-session", 3, {}, answer.callback());
            QTRY_VERIFY(!commandOf(relay, "session.pending_approvals").isEmpty());
            id = commandOf(relay, "session.pending_approvals");
            // In flight: nothing waits in the outbox, nothing is on disk.
            QVERIFY(backend.pendingCommands().isEmpty());
            QFile file(outbox);
            QVERIFY(!file.exists() ||
                    (file.open(QIODevice::ReadOnly) && !file.readAll().contains(id.toUtf8())));
            relay.hostPaused = false;
            QTRY_VERIFY(answer.done);
            QVERIFY2(answer.error.isEmpty(), qPrintable(answer.error));
            QCOMPARE(answer.result.value("approvals").toArray().size(), 2);
            QCOMPARE(completed.size(), 0);
            QVERIFY(!QFile::exists(outbox));
        }
        // A restart replays nothing.
        const auto submitted = relay.submissions.value(id);
        RelayMachineBackend backend(&client, "linux-host", fastOptions(outbox));
        connectAndWait(backend);
        QTest::qWait(100);
        QCOMPARE(relay.submissions.value(id), submitted);
    }

    void aRefusalWithoutACodeFailsTheCommandNotTheLink() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        Answer answer;
        backend.listConversations({}, answer.callback());
        QTRY_VERIFY(answer.done);
        QSignalSpy states(&backend, &MachineBackend::connectionStateChanged);
        const auto sentSoFar = [&relay] {
            int sent = 0;
            for (auto it = relay.submissions.cbegin(); it != relay.submissions.cend(); ++it)
                sent += it.value();
            return sent;
        };
        const auto before = sentSoFar();
        relay.refusedKinds.insert("session.start_turn");
        backend.startTurn("opaque-session", 3, {{"prompt", "refused"}}, answer.callback());
        QTRY_VERIFY(answer.done);
        QCOMPARE(answer.error, QStringLiteral("Control refused the request."));
        const auto id = commandOf(relay, "session.start_turn");
        QVERIFY(id.isEmpty());
        QTest::qWait(200);
        // Still Connected, nothing waiting, and the relay was asked once.
        QCOMPARE(backend.connectionState(), ConnectionState::Connected);
        QCOMPARE(states.size(), 0);
        QVERIFY(backend.pendingCommands().isEmpty());
        QCOMPARE(sentSoFar() - before, 1);
    }

    void aServerErrorWithoutACodeKeepsTheCommandForTheLink_data() {
        QTest::addColumn<int>("status");
        QTest::newRow("503") << 503;
        QTest::newRow("408") << 408;
        QTest::newRow("429") << 429;
    }

    void aServerErrorWithoutACodeKeepsTheCommandForTheLink() {
        QFETCH(int, status);
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        Answer listed;
        backend.listConversations({}, listed.callback());
        QTRY_VERIFY(listed.done);
        QSignalSpy completed(&backend, &RelayMachineBackend::commandCompleted);
        relay.unavailableKinds.insert("session.start_turn", status);
        Answer answer;
        backend.startTurn("opaque-session", 3, {{"prompt", "later"}}, answer.callback());
        // A server error, timeout or rate limit without a code is the link: the command stays
        // stored and is sent again.
        QTRY_COMPARE(backend.pendingCommands().size(), 1);
        const auto id = backend.pendingCommands().first();
        QTRY_VERIFY(relay.submissions.value(id) >= 2);
        relay.unavailableKinds.clear();
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(relay.executions.value(id), 1);
        QVERIFY(backend.pendingCommands().isEmpty());
    }

    void eventsArePolledOnlyForOpenConversations() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        QStringList ids{"opaque-session"};
        for (int i = 1; i < 20; ++i) {
            ids.append(QStringLiteral("session-%1").arg(i));
            relay.addSession(ids.last());
        }
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        Answer listed;
        backend.listConversations({}, listed.callback());
        QTRY_VERIFY(listed.done);
        QCOMPARE(listed.result.value("sessions").toArray().size(), ids.size());
        const auto polls = [&relay] {
            int count = 0;
            for (auto it = relay.eventPolls.cbegin(); it != relay.eventPolls.cend(); ++it)
                count += it.value();
            return count;
        };
        // Twenty distinct conversations, each opened (a write follows its events) and closed,
        // twice over: once all are closed, no events are polled at all.
        for (int round = 0; round < 2; ++round)
            for (const auto& id : std::as_const(ids)) {
                Answer stopped;
                backend.stopTurn(id, 3, stopped.callback());
                QTRY_VERIFY(stopped.done);
                QVERIFY2(stopped.error.isEmpty(), qPrintable(stopped.error));
                QTRY_VERIFY(relay.eventPolls.value(id) > 0);
                backend.conversationClosed(id);
            }
        QTest::qWait(60);
        const auto closed = polls();
        QTest::qWait(300);
        QCOMPARE(polls(), closed);
        // A read on a closed conversation does not start polling it again.
        Answer read;
        backend.pendingApprovals("session-3", 3, {}, read.callback());
        QTRY_VERIFY(read.done);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QTest::qWait(200);
        QCOMPARE(polls(), closed);
        // One open conversation: about one request per poll interval (20 ms), for it alone.
        Answer stopped;
        backend.stopTurn("session-7", 3, stopped.callback());
        QTRY_VERIFY(stopped.done);
        const auto open = polls();
        const auto others = open - relay.eventPolls.value("session-7");
        QTest::qWait(400);
        const auto rate = polls() - open;
        QVERIFY2(rate >= 1 && rate <= 25, qPrintable(QString::number(rate)));
        QCOMPARE(polls() - relay.eventPolls.value("session-7"), others);
    }

    void aWorktreeGrantedLaterAppearsWithoutANewSignIn() {
        QTemporaryDir dir;
        FixtureTransport network;
        ControlV2Client client(QUrl{origin});
        EncryptedFixture fixture(client, network);
        SyntheticRelay relay(network, fixture, client);
        RemoteControlTestAccess::restorePoll(client, 10);
        RemoteControlTestAccess::verifyRetry(client, 100);
        relay.worktrees.append(worktreeDto("wt-locked", "locked"));
        relay.ungranted.insert("wt-locked");
        RelayMachineBackend backend(&client, "linux-host",
                                    fastOptions(dir.filePath("outbox.json")));
        connectAndWait(backend);
        QJsonObject listing;
        settledListing(backend, &listing);
        QCOMPARE(rejection(listing, "wt-locked"), "worktree_not_granted");
        // The grant arrives; the same scope is asked again once the verdict expires.
        relay.ungranted.clear();
        QTest::qWait(150);
        settledListing(backend, &listing);
        QCOMPARE(worktreeNamed(listing, "wt-locked").value("verified").toString(), "host_read");
        QVERIFY(rejection(listing, "wt-locked").isEmpty());
    }
};

QTEST_GUILESS_MAIN(RemoteRelayBackendTest)
#include "RemoteRelayBackendTest.moc"
