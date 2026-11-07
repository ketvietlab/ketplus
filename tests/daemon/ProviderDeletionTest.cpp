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
                                   "BEGIN SELECT RAISE(IGNORE); END")
                               .arg(fault)));
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
    void newDeletionDefersWhenOwnershipCannotBeRead() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession session;
        session.id = "first";
        session.generation = 1;
        session.adapter = "codex";
        session.threadId = "shared-thread";
        session.worktreePath = dir.path();
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        session.id = "second";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec("ALTER TABLE sessions RENAME COLUMN provider_threads TO hidden_threads"));
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete-with-unreadable-ownership"},
                                                {"message_type", "session.delete"},
                                                {"session_id", "first"},
                                                {"session_generation", 1},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY(q.exec("ALTER TABLE sessions RENAME COLUMN hidden_threads TO provider_threads"));
        QVERIFY2(!result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        QCOMPARE(journal.sessions().size(), 2);
        QVERIFY(journal.sessionTombstone("first", 1).sessionId.isEmpty());
        QVERIFY(journal.deletionWork().isEmpty());
        const auto retry = host.handleCommand({{"protocol_version", 0},
                                               {"message_id", "delete-after-repair"},
                                               {"message_type", "session.delete"},
                                               {"session_id", "first"},
                                               {"session_generation", 1},
                                               {"payload", QJsonObject{}}},
                                              "desktop");
        QVERIFY(retry.value("ok").toBool());
        QCOMPARE(journal.sessions().size(), 1);
        QCOMPARE(journal.sessions().first().id, QString("second"));
        QVERIFY(journal.deletionWork()
                    .first()
                    .toObject()
                    .value("provider_targets")
                    .toArray()
                    .isEmpty());
    }
    void queuedCleanupDefersWhenOwnershipCannotBeRead_data() {
        QTest::addColumn<bool>("partial");
        QTest::newRow("query-error") << false;
        QTest::newRow("partial-row-error") << true;
    }
    void queuedCleanupDefersWhenOwnershipCannotBeRead() {
        QFETCH(bool, partial);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        const auto executable = dir.filePath("codex");
        const auto log = dir.filePath("codex.log");
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal journal(path);
        StoredSession session;
        session.id = "deleted";
        session.generation = 1;
        session.adapter = "codex";
        session.threadId = "shared-thread";
        session.worktreePath = dir.path();
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        QString error;
        QVERIFY(journal.deleteSession(
            "deleted", 1, "op", 1, &error,
            {QJsonObject{{"provider", "codex"}, {"thread_id", "shared-thread"}}}));
        session.id = "z-retained";
        QVERIFY(journal.upsertSession(session));
        session.id = "a-readable";
        session.threadId = "unrelated-thread";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter(executable);
        bool warm = false;
        adapter.call("model/list", {}, [&](const QJsonObject&, const QString& e) {
            QVERIFY2(e.isEmpty(), qPrintable(e));
            warm = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(warm, 5000);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        {
            SessionHost host(&journal, &adapter);
            if (partial) {
                QVERIFY(q.exec("PRAGMA table_info(sessions)"));
                QStringList columns;
                while (q.next()) {
                    const auto column = q.value(1).toString();
                    columns.append(
                        column == "provider_threads"
                            ? "CASE WHEN id='z-retained' THEN json_extract('invalid','$') "
                              "ELSE provider_threads END AS provider_threads"
                            : '"' + column + '"');
                }
                q.finish();
                QVERIFY(q.exec("ALTER TABLE sessions RENAME TO held_sessions"));
                QVERIFY(q.exec("CREATE VIEW sessions AS SELECT " + columns.join(',') +
                               " FROM held_sessions"));
            } else {
                QVERIFY(q.exec(
                    "ALTER TABLE sessions RENAME COLUMN provider_threads TO hidden_threads"));
            }
            // Run the queued maintenance, then a provider barrier. Any earlier
            // thread/delete must finish before that barrier response arrives.
            QCoreApplication::sendPostedEvents();
            bool barrier = false;
            adapter.call("model/list", {}, [&](const QJsonObject&, const QString& e) {
                QVERIFY2(e.isEmpty(), qPrintable(e));
                barrier = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(barrier, 5000);
            QFile calls(log);
            QVERIFY(calls.open(QIODevice::ReadOnly));
            const auto recorded = calls.readAll();
            QVERIFY2(!recorded.contains("delete:shared-thread"), recorded.constData());
            QCOMPARE(journal.deletionWork()
                         .first()
                         .toObject()
                         .value("provider_targets")
                         .toArray()
                         .size(),
                     1);
            if (partial) {
                QVERIFY(q.exec("DROP VIEW sessions"));
                QVERIFY(q.exec("ALTER TABLE held_sessions RENAME TO sessions"));
            } else {
                QVERIFY(q.exec(
                    "ALTER TABLE sessions RENAME COLUMN hidden_threads TO provider_threads"));
            }
        }
        SessionHost restarted(&journal, &adapter);
        QTRY_VERIFY(journal.deletionWork()
                        .first()
                        .toObject()
                        .value("provider_targets")
                        .toArray()
                        .isEmpty());
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QVERIFY(!calls.readAll().contains("delete:shared-thread"));
        QCOMPARE(journal.sessions().size(), 2);
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
