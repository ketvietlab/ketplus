#include "DaemonClient.h"
#include "TranscriptWindowTestSupport.h"

#include <QStandardPaths>

class DesktopSessionDiscoveryTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void discoversAnotherClientThroughRealDaemon() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"desktop", path, "codex", {}, {}, "ready", 1, 0, 0, "Desktop"}));
        }
        rememberConversation(path, "desktop");
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(!server.isRestoring());
        const auto selected = desktop.activeConversationId(path);
        remote::DaemonClient phone;
        bool finished = false;
        QString failure;
        phone.call("session.start_turn", {{"worktree_path", path}, {"prompt", "hello"}},
                   "phone-created", 1, [&](const QJsonObject&, const QString& error) {
                       failure = error;
                       finished = true;
                   });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QTRY_COMPARE_WITH_TIMEOUT(desktop.conversations(path).size(), 2, 5000);
        QCOMPARE(desktop.activeConversationId(path), selected);
        const auto phoneRow = [&]() {
            for (const auto& value : desktop.conversations(path)) {
                if (value.toMap().value("id") == "phone-created")
                    return value.toMap();
            }
            return QVariantMap{};
        };
        QVERIFY(!phoneRow().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!phoneRow().value("running").toBool(), 5000);
        QVERIFY(!phoneRow().value("seen").toBool());
        desktop.selectConversation(path, "phone-created");
        QTest::qWait(150);
        desktop.selectConversation(path, selected);
        QTest::qWait(150);
        QVERIFY(phoneRow().value("seen").toBool());
        // Reading one completion must not hide a subsequent background completion.
        finished = false;
        phone.call("session.start_turn", {{"worktree_path", path}, {"prompt", "again"}},
                   "phone-created", 1, [&](const QJsonObject&, const QString& error) {
                       failure = error;
                       finished = true;
                   });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QTRY_VERIFY_WITH_TIMEOUT(!phoneRow().value("seen").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!phoneRow().value("running").toBool(), 5000);
        desktop.selectConversation(path, "phone-created");
        QTest::qWait(150);
        desktop.selectConversation(path, selected);
        QTest::qWait(150);
        QVERIFY(phoneRow().value("seen").toBool());
    }

    void discoversPhoneConversation_data() {
        QTest::addColumn<bool>("otherWorkspace");
        QTest::addColumn<bool>("duringInitialList");
        QTest::newRow("same-workspace") << false << false;
        QTest::newRow("other-workspace") << true << false;
        QTest::newRow("during-initial-list") << false << true;
    }

    void discoversPhoneConversation() {
        QFETCH(bool, otherWorkspace);
        QFETCH(bool, duringInitialList);
        Environment env;
        QVERIFY(writeToken(env.directory));
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        QVERIFY(QDir(path).mkpath("other"));
        const auto phonePath = otherWorkspace ? path + "/other" : path;
        rememberConversation(path, "desktop");
        bool phoneExists = false;
        bool sentGap = false;
        int lists = 0;
        FakeDaemon* fake = nullptr;
        const auto stored = [](const QString& id, const QString& cwd) {
            return QJsonObject{
                {"session_id", id},    {"worktree_path", cwd},    {"conversation_path", cwd},
                {"adapter", "claude"}, {"session_generation", 1}, {"context_revision", 1},
                {"title", id},         {"state", "ready"},        {"last_event_cursor", 1}};
        };
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& request) -> QJsonObject {
                if (type == "session.list") {
                    ++lists;
                    QJsonArray sessions{stored("desktop", path)};
                    if (phoneExists) {
                        sessions.append(stored("phone", phonePath));
                        // A live event beats the reply to an older catalog snapshot.
                        if (!sentGap) {
                            sentGap = true;
                            fake->push(liveEvent("phone", "turn.started",
                                                 {{"turn_id", "phone-turn"}}, 2, 1));
                        }
                    } else if (duringInitialList) {
                        phoneExists = true;
                        fake->push(liveEvent("phone", "session.created", {}, 1, 0));
                    }
                    return {{"transcript_window_version", 1}, {"sessions", sessions}};
                }
                const auto id = request.value("session_id").toString();
                if (type == "session.transcript_window")
                    return {{"transcript_window_version", 1},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"snapshot_cursor", 1},
                            {"projection_revision", 1},
                            {"before_cursor", 1},
                            {"has_more_before", false},
                            {"events", QJsonArray{}},
                            {"state", "ready"},
                            {"pending_approvals", QJsonArray{}}};
                if (type == "session.attach")
                    return {{"session_id", id},
                            {"session_generation", 1},
                            {"next_event_cursor", 2},
                            {"last_event_cursor", 2},
                            {"events", QJsonArray{windowEvent(id, "turn.started",
                                                              {{"turn_id", "phone-turn"}}, 2, 1)}}};
                return {};
            });
        fake = &daemon;
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy discovered(&server, &CodexAppServer::daemonSessionDiscovered);
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(daemon.received.contains("session.transcript_window"));
        QTRY_VERIFY(!desktop.conversations(path).isEmpty());
        QTRY_VERIFY(!server.isRestoring());
        const auto active = desktop.activeConversationId(path);
        const auto windowReads = daemon.received.count("session.transcript_window");
        if (!duringInitialList) {
            phoneExists = true;
            for (int i = 0; i < 20; ++i)
                daemon.push(liveEvent("phone", "session.created", {}, 1, 0));
        }
        QTRY_COMPARE(desktop.conversations(phonePath).size(), otherWorkspace ? 1 : 2);
        QTRY_VERIFY(daemon.received.contains("session.attach"));
        QCOMPARE(desktop.activeConversationId(path), active);
        QCOMPARE(lists, 2); // Event burst is coalesced into one incremental catalog read.
        QCOMPARE(discovered.size(), 2); // The existing conversation was not re-discovered.
        QCOMPARE(daemon.received.count("session.transcript_window"), windowReads);
        bool recoveredGap = false;
        for (const auto& request : daemon.envelopes) {
            if (request.value("message_type") == "session.attach") {
                QCOMPARE(request.value("session_id").toString(), QString("phone"));
                QCOMPARE(request.value("payload").toObject().value("after_cursor").toInteger(), 1);
                recoveredGap = true;
            }
        }
        QVERIFY(recoveredGap);
        QTest::qWait(50);
        QCOMPARE(lists, 2);
    }
};

QTEST_GUILESS_MAIN(DesktopSessionDiscoveryTest)
#include "DesktopSessionDiscoveryTest.moc"
