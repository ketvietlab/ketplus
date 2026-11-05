#include "FakeCodex.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
class ProviderDeletionTest final : public QObject {
    Q_OBJECT
  private slots:
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
