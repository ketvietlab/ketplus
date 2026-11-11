#include "DaemonClient.h"
#include "TranscriptWindowTestSupport.h"

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>

class DesktopSessionDiscoveryTest final : public QObject {
    Q_OBJECT

    static QString archivedAt(const QString& journalPath, const QString& id) {
        const auto name = "archive-probe";
        QString value = "<missing>";
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(journalPath);
            if (db.open()) {
                QSqlQuery q(db);
                q.prepare("SELECT archived_at FROM sessions WHERE id=?");
                q.addBindValue(id);
                if (q.exec() && q.next())
                    value = q.value(0).toString();
            }
        }
        QSqlDatabase::removeDatabase(name);
        return value;
    }

    static bool isArchived(const AgentSessionManager& desktop, const QString& id) {
        for (const auto& value : desktop.archivedConversations())
            if (value.toMap().value("id") == id)
                return true;
        return false;
    }

    // "target" and "kept" in the journal and in this desktop's settings, already in agreement.
    static void seedArchiveJournal(const QString& journalPath, const QString& path, bool archived) {
        LocalJournal journal(journalPath);
        for (const auto* id : {"target", "kept"})
            QVERIFY(journal.upsertSession(
                {id, path, "codex", {}, {}, "ready", 1, 0, 0, QString::fromLatin1(id)}));
        if (archived)
            QVERIFY(journal.setSessionArchived(
                "target", 1,
                QDateTime::currentDateTimeUtc().addDays(-31).toString(Qt::ISODateWithMs)));
        const auto row = [](const char* id, bool value) {
            return QVariantMap{{"id", id}, {"threadId", id}, {"title", id}, {"archived", value}};
        };
        QSettings().setValue(persistedKey(path),
                             QVariantMap{{"path", path},
                                         {"activeId", "kept"},
                                         {"conversations", QVariantList{row("target", archived),
                                                                        row("kept", false)}}});
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void discoversAnotherClientThroughRealDaemon_data() {
        QTest::addColumn<int>("tombstones");
        QTest::newRow("empty") << 0;
        QTest::newRow("20000") << 20000;
        QTest::newRow("100000") << 100000;
    }

    void discoversAnotherClientThroughRealDaemon() {
        QFETCH(int, tombstones);
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
        if (tombstones) {
            const auto name = "discovery-backlog";
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", name);
                db.setDatabaseName(env.directory.filePath("journal.sqlite3"));
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec(
                    QString(
                        "WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<%1) "
                        "INSERT INTO session_tombstones SELECT 'deleted-'||i,1,1,'op','now' FROM n")
                        .arg(tombstones)));
            }
            QSqlDatabase::removeDatabase(name);
        }
        rememberConversation(path, "desktop");
        DaemonService service(
            {env.directory.filePath("journal.sqlite3"), env.directory.filePath("local.token"),
             env.directory.filePath("daemon.sock"), executable,
             env.directory.filePath("missing-claude"), env.directory.filePath("missing-cursor")});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        int deletions = 0;
        connect(&server, &CodexAppServer::daemonSessionDeleted, &server,
                [&](const QString&, qint64) { ++deletions; });
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(!server.isRestoring());
        QVERIFY(deletions >= tombstones); // Initial refresh still reconciles retained deletions.
        const auto initialDeletions = deletions;
        const auto selected = desktop.activeConversationId(path);
        QElapsedTimer timer;
        timer.start();
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
        QCOMPARE(deletions, initialDeletions); // Discovery must not replay deletion history.
        qInfo() << "live discovery" << tombstones << "retained tombstones:" << timer.elapsed()
                << "ms, additional deletion signals:" << deletions - initialDeletions;
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

    void archiveStateReachesTheDaemon() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        const auto journalPath = env.directory.filePath("journal.sqlite3");
        {
            LocalJournal journal(journalPath);
            for (const auto* id : {"archived-here", "kept", "archived-elsewhere"})
                QVERIFY(journal.upsertSession(
                    {id, path, "codex", {}, {}, "ready", 1, 0, 0, QString::fromLatin1(id)}));
            QVERIFY(
                journal.setSessionArchived("archived-elsewhere", 1, "2026-01-01T00:00:00.000Z"));
        }
        // Archived before this desktop told the daemon; the first catalog sends it once.
        const auto conversation = [](const char* id, bool archived) {
            return QVariantMap{{"id", id}, {"threadId", id}, {"title", id}, {"archived", archived}};
        };
        QSettings().setValue(
            persistedKey(path),
            QVariantMap{{"path", path},
                        {"activeId", "kept"},
                        {"conversations", QVariantList{conversation("archived-here", true),
                                                       conversation("kept", false)}}});
        const auto archivedAt = [&](const QString& id) {
            return DesktopSessionDiscoveryTest::archivedAt(journalPath, id);
        };
        const auto archivedIds = [](const QVariantList& values) {
            QStringList ids;
            for (const auto& value : values)
                ids.append(value.toMap().value("id").toString());
            ids.sort();
            return ids;
        };
        DaemonService service({journalPath, env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable,
                               env.directory.filePath("missing-claude"),
                               env.directory.filePath("missing-cursor")});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(!server.isRestoring());
        QVERIFY(server.hasArchiveSupport());
        QTRY_VERIFY_WITH_TIMEOUT(!archivedAt("archived-here").isEmpty(), 5000);
        // A conversation this desktop first sees keeps the daemon's archive state and time.
        QTRY_COMPARE_WITH_TIMEOUT(archivedIds(desktop.archivedConversations()),
                                  (QStringList{"archived-elsewhere", "archived-here"}), 5000);
        QCOMPARE(archivedAt("archived-elsewhere"), QString("2026-01-01T00:00:00.000Z"));
        QCOMPARE(archivedAt("kept"), QString());
        QCOMPARE(desktop.activeConversationId(path), QString("kept"));

        QVERIFY(desktop.restoreConversation(path, "archived-here"));
        QTRY_VERIFY_WITH_TIMEOUT(archivedAt("archived-here").isEmpty(), 5000);
        QVERIFY(desktop.archiveConversation(path, "kept"));
        QTRY_VERIFY_WITH_TIMEOUT(!archivedAt("kept").isEmpty(), 5000);
        desktop.archiveConversationsForWorktree(path);
        QTRY_VERIFY_WITH_TIMEOUT(!archivedAt("archived-here").isEmpty(), 5000);
        QCOMPARE(archivedAt("archived-elsewhere"), QString("2026-01-01T00:00:00.000Z"));
    }

    // ADR 0117: the daemon keeps conversation tags; the desktop lists none and offers no way to
    // change them. Another client's change still reaches the desktop as `tags.changed`, which
    // it takes without an error or a warning.
    void tagsStayInTheDaemon() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        const auto journalPath = env.directory.filePath("journal.sqlite3");
        {
            LocalJournal journal(journalPath);
            QVERIFY(journal.upsertSession(
                {"tagged", path, "codex", {}, {}, "ready", 1, 0, 0, QStringLiteral("tagged")}));
            QVERIFY(journal.setSessionTags("tagged", 1, {"Review"}));
        }
        const auto storedTags = [&] {
            for (const auto& session : LocalJournal(journalPath).sessions())
                if (session.id == QStringLiteral("tagged"))
                    return session.tags;
            return QStringList{"missing"};
        };
        DaemonService service({journalPath, env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable,
                               env.directory.filePath("missing-claude"),
                               env.directory.filePath("missing-cursor")});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        const auto row = [&] {
            for (const auto& value : desktop.conversations(path))
                if (value.toMap().value("id").toString() == QStringLiteral("tagged"))
                    return value.toMap();
            return QVariantMap{};
        };
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(!server.isRestoring());
        QTRY_VERIFY_WITH_TIMEOUT(!row().isEmpty(), 5000);
        QVERIFY(!row().contains("tags"));
        QVERIFY(!row().contains("tagsEditable"));

        QSignalSpy errors(&desktop, &AgentSessionManager::errorOccurred);
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
        remote::DaemonClient watcher;
        remote::DaemonClient other;
        QTRY_VERIFY(watcher.isReady() && other.isReady());
        QSignalSpy pushed(&watcher, &remote::DaemonClient::noticeReceived);
        // The desktop's own connection: its handlers run while the notice is emitted.
        auto* desktopClient = server.findChild<remote::DaemonClient*>();
        QVERIFY(desktopClient != nullptr);
        QSignalSpy desktopNotices(desktopClient, &remote::DaemonClient::noticeReceived);
        const auto desktopReceived = [&] {
            for (const auto& notice : desktopNotices)
                if (notice.first().toString() == QStringLiteral("tags.changed"))
                    return true;
            return false;
        };
        bool saved = false;
        other.call(QStringLiteral("tags.catalog.set"),
                   {{"tags", QJsonArray{QJsonObject{{"label", "Reviewed"}, {"color", "#0e7c86"}}}},
                    {"renames", QJsonObject{{"Review", "Reviewed"}}}},
                   {}, -1, [&](const QJsonObject&, const QString& error) {
                       QVERIFY2(error.isEmpty(), qPrintable(error));
                       saved = true;
                   });
        QTRY_VERIFY_WITH_TIMEOUT(saved, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!pushed.isEmpty(), 5000);
        QCOMPARE(pushed.first().first().toString(), QStringLiteral("tags.changed"));
        QCOMPARE(storedTags(), QStringList{"Reviewed"});
        // The same push reaches the desktop's own connection and passes without an error.
        QTRY_VERIFY_WITH_TIMEOUT(desktopReceived(), 5000);
        QVERIFY(errors.isEmpty());
        QVERIFY(!row().contains("tags"));
    }

    void anotherDesktopsArchiveIsNotUndone_data() {
        QTest::addColumn<bool>("archived");
        QTest::newRow("archive") << false;
        QTest::newRow("restore") << true;
    }

    void anotherDesktopsArchiveIsNotUndone() {
        QFETCH(bool, archived);
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        const auto journalPath = env.directory.filePath("journal.sqlite3");
        seedArchiveJournal(journalPath, path, archived);
        DaemonService service({journalPath, env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable,
                               env.directory.filePath("missing-claude"),
                               env.directory.filePath("missing-cursor")});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer serverA;
        CodexAppServer serverB;
        AgentSessionManager a(&serverA);
        AgentSessionManager b(&serverB);
        a.activate(path);
        b.activate(path);
        QTRY_VERIFY(serverA.isReady() && serverB.isReady());
        QTRY_VERIFY(!serverA.isRestoring() && !serverB.isRestoring());
        QTRY_COMPARE(archivedAt(journalPath, "target").isEmpty(), !archived);
        if (archived)
            QVERIFY(a.restoreConversation(path, "target"));
        else
            QVERIFY(a.archiveConversation(path, "target"));
        QTRY_COMPARE(archivedAt(journalPath, "target").isEmpty(), archived);
        // B made no change of its own; its full catalog refresh adopts A's state.
        auto* client = serverB.findChild<remote::DaemonClient*>();
        QVERIFY(client);
        int listed = 0;
        connect(&serverB, &CodexAppServer::daemonSessionDiscovered, &serverB,
                [&](const QJsonObject& value) {
                    if (value.value("session_id") == "target")
                        ++listed;
                });
        emit client->readyChanged(false);
        emit client->readyChanged(true);
        QTRY_VERIFY(listed > 0);
        bool barrier = false;
        client->call("session.list", {}, {}, -1,
                     [&](const QJsonObject&, const QString&) { barrier = true; });
        QTRY_VERIFY(barrier);
        QCOMPARE(archivedAt(journalPath, "target").isEmpty(), archived);
        QCOMPARE(isArchived(b, "target"), !archived);
    }

    void failedRestoreKeepsTheConversationArchived() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        const auto journalPath = env.directory.filePath("journal.sqlite3");
        seedArchiveJournal(journalPath, path, true);
        DaemonService service({journalPath, env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable,
                               env.directory.filePath("missing-claude"),
                               env.directory.filePath("missing-cursor")});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy errors(&desktop, &AgentSessionManager::errorOccurred);
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(!server.isRestoring());
        QTRY_VERIFY(isArchived(desktop, "target"));
        const auto sql = [&](const QString& statement) {
            const auto name = "archive-trigger";
            bool ok = false;
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", name);
                db.setDatabaseName(journalPath);
                ok = db.open() && QSqlQuery(db).exec(statement);
            }
            QSqlDatabase::removeDatabase(name);
            return ok;
        };
        QVERIFY(sql("CREATE TRIGGER reject_restore BEFORE UPDATE OF archived_at ON sessions "
                    "WHEN NEW.archived_at='' BEGIN SELECT RAISE(ABORT,'rejected'); END"));
        QVERIFY(desktop.restoreConversation(path, "target"));
        QTRY_COMPARE(errors.count(), 1);
        QVERIFY(errors.first().first().toString().contains("not restored"));
        QVERIFY(isArchived(desktop, "target"));
        QVERIFY(!archivedAt(journalPath, "target").isEmpty());
        QVERIFY(sql("DROP TRIGGER reject_restore"));
        QVERIFY(desktop.restoreConversation(path, "target"));
        QTRY_VERIFY(!isArchived(desktop, "target"));
        QCOMPARE(archivedAt(journalPath, "target"), QString());
    }

    void discoversPhoneConversation_data() {
        QTest::addColumn<bool>("otherWorkspace");
        QTest::addColumn<bool>("duringInitialList");
        QTest::addColumn<bool>("olderPagedDaemon");
        QTest::newRow("legacy-same-workspace") << false << false << false;
        QTest::newRow("legacy-other-workspace") << true << false << false;
        QTest::newRow("legacy-during-initial-list") << false << true << false;
        QTest::newRow("paged-same-workspace") << false << false << true;
        QTest::newRow("paged-other-workspace") << true << false << true;
        QTest::newRow("paged-during-initial-list") << false << true << true;
    }

    void discoversPhoneConversation() {
        QFETCH(bool, otherWorkspace);
        QFETCH(bool, duringInitialList);
        QFETCH(bool, olderPagedDaemon);
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
                    QJsonObject catalog{{"transcript_window_version", 1}, {"sessions", sessions}};
                    if (olderPagedDaemon && lists > 1) {
                        // A daemon predating live-only ignores the opt-in and returns
                        // page one with sessions. Discovery must not walk its next page.
                        const QJsonObject deleted{{"session_id", "deleted"},
                                                  {"session_generation", 1}};
                        catalog.insert("deleted_sessions_page_version", 1);
                        catalog.insert("deleted_sessions_snapshot", "old-daemon-token");
                        catalog.insert("deleted_sessions", QJsonArray{deleted});
                        catalog.insert("deleted_sessions_next", deleted);
                    }
                    return catalog;
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
        QSignalSpy deleted(&server, &CodexAppServer::daemonSessionDeleted);
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
        QCOMPARE(deleted.size(), 0);
        QCOMPARE(daemon.received.count("session.transcript_window"), windowReads);
        bool recoveredGap = false;
        int catalogReads = 0;
        for (const auto& request : daemon.envelopes) {
            if (request.value("message_type") == "session.list" && ++catalogReads > 1) {
                const auto payload = request.value("payload").toObject();
                QVERIFY(payload.value("session_catalog_live_only").toBool());
                QCOMPARE(payload.value("deleted_sessions_page_version").toInt(), 1);
            }
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
