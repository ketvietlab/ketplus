#include "FakeCodex.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
class ProviderDeletionTest final : public QObject {
    Q_OBJECT
  private slots:
    void failedSessionDeletionRollsBackAndCanRetry_data() {
        QTest::addColumn<QString>("fault");
        QTest::newRow("commit") << "commit";
        QTest::newRow("ignored-tombstone") << "session_tombstones";
        QTest::newRow("ignored-work") << "session_deletion_work";
        QTest::newRow("ignored-history") << "events";
    }
    void failedSessionDeletionRollsBackAndCanRetry() {
        QFETCH(QString, fault);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = "deleted";
        session.generation = 1;
        session.worktreePath = dir.path();
        session.adapter = "codex";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        const auto number = journal.assignConversationNumber(session.id, 1);
        QVERIFY(!number.isEmpty());
        QVERIFY(journal.appendEvent(session.id, 1, "assistant.delta", {{"text", "keep"}}) > 0);
        const auto before = journal.eventsAfter(session.id, 1, 0);
        session.id = "unrelated";
        QVERIFY(journal.upsertSession(session));
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        if (fault == "commit") {
            QVERIFY(q.exec("CREATE TABLE deletion_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE deletion_child(id INTEGER REFERENCES deletion_parent(id) "
                           "DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_delete AFTER DELETE ON sessions "
                           "BEGIN INSERT INTO deletion_child VALUES(1); END"));
        } else if (fault == "events") {
            QVERIFY(q.exec("CREATE TRIGGER fail_delete BEFORE DELETE ON events "
                           "BEGIN SELECT RAISE(IGNORE); END"));
        } else {
            QVERIFY(q.exec(QString("CREATE TRIGGER fail_delete BEFORE INSERT ON %1 "
                                   "BEGIN SELECT RAISE(IGNORE); END").arg(fault)));
        }
        QString error;
        const QJsonArray targets{QJsonObject{{"provider", "codex"}, {"thread_id", "owned"}}};
        QVERIFY(!journal.deleteSession("deleted", 1, "delete-op", 1, &error, targets));
        QVERIFY(!error.isEmpty());
        // A failed commit must release the write transaction, not expose an
        // uncommitted tombstone that would falsely succeed on the next retry.
        QVERIFY2(q.exec("BEGIN"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QCOMPARE(journal.sessions().size(), 2);
        QCOMPARE(journal.eventsAfter("deleted", 1, 0), before);
        QCOMPARE(journal.conversationNumber("deleted", 1), number);
        QVERIFY(journal.sessionTombstone("deleted", 1).sessionId.isEmpty());
        QVERIFY(journal.deletionWork().isEmpty());
        QVERIFY(q.exec("DROP TRIGGER fail_delete"));
        QVERIFY2(journal.deleteSession("deleted", 1, "delete-op", 1, &error, targets),
                 qPrintable(error));
        QCOMPARE(journal.sessions().size(), 1);
        QCOMPARE(journal.sessions().first().id, QString("unrelated"));
        QVERIFY(journal.eventsAfter("deleted", 1, 0).isEmpty());
        QCOMPARE(journal.sessionTombstone("deleted", 1).operationId, QString("delete-op"));
        QCOMPARE(journal.deletionWork().size(), 1);
        QCOMPARE(journal.deletionWork().first().toObject().value("provider_targets").toArray(),
                 targets);
        QVERIFY(journal.deleteSession("deleted", 1, "delete-op", 1, &error, targets));
        QVERIFY(!journal.deleteSession("deleted", 1, "different-op", 1, &error, targets));
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.sessions().size(), 1);
        QCOMPARE(reopened.sessionTombstone("deleted", 1).operationId, QString("delete-op"));
        QCOMPARE(reopened.deletionWork().size(), 1);
    }
    void purgeIsDurableAndScopedToUnsharedProviderThreads() {
        QTemporaryDir directory;
        const auto executable = directory.filePath("codex");
        const auto log = directory.filePath("codex.log");
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "deleted";
        session.adapter = "codex";
        session.threadId = "codex-owned";
        session.worktreePath = directory.path();
        session.state = "ready";
        session.providerThreads =
            R"({"claude":{"thread_id":"claude-old"},"codex":{"retired_thread_ids":["codex-retired"]}})";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        QSignalSpy events(&host, &SessionHost::eventReady);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", session.id},
                                                {"session_generation", session.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY(result.value("ok").toBool());
        QVERIFY(journal.sessions().isEmpty());
        QCOMPARE(journal.deletionWork().size(), 1);
        QCOMPARE(
            journal.deletionWork().first().toObject().value("provider_targets").toArray().size(),
            3);
        QVERIFY(!events.isEmpty());
        QCOMPARE(events.last().first().toJsonObject().value("event_type").toString(),
                 QString("session.deleted"));
        QTRY_COMPARE_WITH_TIMEOUT(
            journal.deletionWork().first().toObject().value("provider_targets").toArray().size(), 1,
            5000);
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto removed = calls.readAll();
        QVERIFY(removed.contains("delete:codex-owned"));
        QVERIFY(removed.contains("delete:codex-retired"));
        const auto deletion =
            journal.deletionWork().first().toObject().value("deletion").toObject();
        QVERIFY(
            !journal.acknowledgeCloudDeletion(session.id, session.generation, "wrong-operation"));
        QVERIFY(journal.acknowledgeCloudDeletion(session.id, session.generation,
                                                 deletion.value("operationId").toString()));
        // Claude was unavailable: keep its exact target, even across a journal reopen.
        LocalJournal reopened(journal.databasePath());
        const auto target = reopened.deletionWork()
                                .first()
                                .toObject()
                                .value("provider_targets")
                                .toArray()
                                .first()
                                .toObject();
        QCOMPARE(target.value("thread_id").toString(), QString("claude-old"));
        QVERIFY(reopened.completeProviderCleanup(session.id, session.generation, "claude",
                                                 "claude-old"));
        QVERIFY(reopened.deletionWork().isEmpty());
    }
    void unavailableProviderCleanupRunsAfterRestart() {
        QTemporaryDir directory;
        const auto executable = directory.filePath("codex");
        const auto log = directory.filePath("codex.log");
        const auto database = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(database);
            StoredSession session;
            session.id = "retry";
            session.worktreePath = directory.path();
            session.adapter = "codex";
            session.threadId = "retry-provider-thread";
            session.state = "ready";
            QVERIFY(journal.upsertSession(session));
            CodexAdapter unavailable(directory.filePath("not-installed"));
            SessionHost host(&journal, &unavailable);
            const auto result = host.handleCommand({{"protocol_version", 0},
                                                    {"message_id", "delete"},
                                                    {"message_type", "session.delete"},
                                                    {"session_id", session.id},
                                                    {"session_generation", session.generation},
                                                    {"payload", QJsonObject{}}},
                                                   "desktop");
            QVERIFY(result.value("ok").toBool());
            QCoreApplication::processEvents();
            QCOMPARE(journal.deletionWork().size(), 1);
        }
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal reopened(database);
        CodexAdapter available(executable);
        SessionHost restarted(&reopened, &available);
        QTRY_VERIFY_WITH_TIMEOUT(reopened.deletionWork()
                                     .first()
                                     .toObject()
                                     .value("provider_targets")
                                     .toArray()
                                     .isEmpty(),
                                 5000);
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QVERIFY(calls.readAll().contains("delete:retry-provider-thread"));
        const auto catalog = restarted.handleCommand({{"protocol_version", 0},
                                                      {"message_id", "list"},
                                                      {"message_type", "session.list"},
                                                      {"payload", QJsonObject{}}},
                                                     "desktop");
        QCOMPARE(catalog.value("deleted_sessions").toArray().size(), 1);
    }
    void unsupportedProviderKeepsItsConversation() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "cursor-owned";
        session.worktreePath = directory.path();
        session.adapter = "cursor";
        session.threadId = "cursor-thread";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", session.id},
                                                {"session_generation", session.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QCOMPARE(result.value("error_code").toString(), "provider_cleanup_unsupported");
        QCOMPARE(journal.sessions().size(), 1);
        QVERIFY(journal.deletionWork().isEmpty());
    }
    void sharedThreadsRemainOwnedByTheOtherConversation() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession first;
        first.id = "first";
        first.adapter = "codex";
        first.threadId = "shared";
        first.worktreePath = directory.path();
        first.state = "ready";
        auto second = first;
        second.id = "second";
        QVERIFY(journal.upsertSession(first));
        QVERIFY(journal.upsertSession(second));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", "first"},
                                                {"session_generation", first.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY(result.value("ok").toBool());
        QVERIFY(journal.deletionWork()
                    .first()
                    .toObject()
                    .value("provider_targets")
                    .toArray()
                    .isEmpty());
        QCOMPARE(journal.sessions().size(), 1);
    }
};
QTEST_GUILESS_MAIN(ProviderDeletionTest)
#include "ProviderDeletionTest.moc"
