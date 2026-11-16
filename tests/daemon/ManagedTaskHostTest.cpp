#include "FakeCodex.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
QJsonObject command(SessionHost& host, const QString& type, const QJsonObject& payload = {},
                    const QString& owner = "owner") {
    auto result = std::make_shared<QJsonObject>();
    host.handleCommandDeferred({{"message_id", QUuid::createUuid().toString()},
                                {"message_type", type},
                                {"session_id", owner},
                                {"session_generation", 1},
                                {"payload", payload}},
                               "local-test",
                               [result](const QJsonObject& reply) { *result = reply; });
    QElapsedTimer timer;
    timer.start();
    while (result->isEmpty() && timer.elapsed() < 6000)
        QTest::qWait(10);
    return *result;
}
bool seed(LocalJournal& journal, const QString& cwd, const QString& id = "owner",
          const QString& policy = "full") {
    StoredSession owner;
    owner.id = id;
    owner.worktreePath = cwd;
    owner.adapter = "codex";
    owner.state = "ready";
    owner.executionPolicy = policy;
    return journal.upsertSession(owner);
}
QJsonObject process(const QString& cwd, const QJsonArray& argv) {
    return {{"client_request_id", "request"},
            {"kind", "process"},
            {"title", "Fixture task"},
            {"timeout_ms", 10000},
            {"execution_spec",
             QJsonObject{{"argv", argv}, {"cwd", cwd}, {"env_delta", QJsonObject{}}}}};
}
int turns(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    int count = 0;
    for (const auto& line : file.readAll().split('\n'))
        if (line.startsWith("turn:"))
            ++count;
    return count;
}
QJsonObject task(SessionHost& host, const QString& id) {
    return command(host, "session.task.get", {{"task_id", id}}).value("task").toObject();
}
} // namespace

class ManagedTaskHostTest final : public QObject {
    Q_OBJECT
  private slots:
    void ownerApprovalHoldsCompletionUntilTheDecisionIsAnswered() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        QVERIFY(command(host, "session.start_turn", {{"prompt", "approve"}}).value("ok").toBool());
        QTRY_VERIFY(!command(host, "session.pending_approvals")
                         .value("pending_approvals")
                         .toArray()
                         .isEmpty());
        const auto approval = command(host, "session.pending_approvals")
                                  .value("pending_approvals")
                                  .toArray()
                                  .first()
                                  .toObject();
        const auto started =
            command(host, "session.task.start", process(cwd, {"/usr/bin/printf", "done"}));
        const auto id = started.value("task").toObject().value("task_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "completed", 6000);
        QTest::qWait(700);
        QCOMPARE(task(host, id).value("completion").toObject().value("status"), "pending");
        QCOMPARE(turns(log), 1);
        QVERIFY(journal.backgroundTasks().deliveryPage(true).value("events").toArray().isEmpty());
        QVERIFY(command(host, "approval.resolve",
                        {{"approval_id", approval.value("approval_id")}, {"decision", "accept"}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 2, 6000);
        QCOMPARE(task(host, id).value("completion").toObject().value("status"), "delivered");
    }

    void busyOwnerEnqueuesCompletionWithoutSteeringItsCurrentTurn() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        QVERIFY(command(host, "session.start_turn", {{"prompt", "hang"}}).value("ok").toBool());
        QTRY_COMPARE(turns(log), 1);
        const auto started =
            command(host, "session.task.start", process(cwd, {"/usr/bin/printf", "done"}));
        const auto id = started.value("task").toObject().value("task_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("completion").toObject().value("status"),
                                  "delivered", 6000);
        QCOMPARE(turns(log), 1);
        QVERIFY(!journal.backgroundTasks().deliveryPage(true).value("events").toArray().isEmpty());
        StoredSession current;
        for (const auto& session : journal.sessions())
            if (session.id == "owner")
                current = session;
        emit adapter.notificationReceived(
            "turn/completed",
            {{"threadId", current.threadId},
             {"turn", QJsonObject{{"id", current.turnId}, {"status", "completed"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 2, 6000);
        QTest::qWait(700);
        QCOMPARE(turns(log), 2);
    }

    void childKeepsItsOwnApprovalBrokerAfterOwnerApprovesStart() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd, "owner", "ask"));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        const QJsonObject input{{"client_request_id", "child"},
                                {"kind", "agent"},
                                {"title", "Own approval"},
                                {"timeout_ms", 15000},
                                {"execution_spec", QJsonObject{{"prompt", "approve"}}}};
        const auto started = command(host, "session.task.start", input);
        const auto id = started.value("task").toObject().value("task_id").toString();
        QVERIFY2(!id.isEmpty(), qPrintable(QJsonDocument(started).toJson()));
        QTRY_VERIFY(!command(host, "session.pending_approvals")
                         .value("pending_approvals")
                         .toArray()
                         .isEmpty());
        const auto approval = command(host, "session.pending_approvals")
                                  .value("pending_approvals")
                                  .toArray()
                                  .first()
                                  .toObject();
        QVERIFY(command(host, "approval.resolve",
                        {{"approval_id", approval.value("approval_id")}, {"decision", "accept"}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "running", 5000);
        const auto child = task(host, id)
                               .value("execution_identity")
                               .toObject()
                               .value("child_session_id")
                               .toString();
        QVERIFY(child != "owner");
        QTRY_VERIFY_WITH_TIMEOUT(!command(host, "session.pending_approvals", {}, child)
                                      .value("pending_approvals")
                                      .toArray()
                                      .isEmpty(),
                                 5000);
        QCOMPARE(task(host, id).value("state"), "running");
        const auto childApproval = command(host, "session.pending_approvals", {}, child)
                                       .value("pending_approvals")
                                       .toArray()
                                       .first()
                                       .toObject();
        QVERIFY(childApproval.value("approval_id") != approval.value("approval_id"));
        QCOMPARE(turns(log), 1);
        QVERIFY(
            command(host, "approval.resolve",
                    {{"approval_id", childApproval.value("approval_id")}, {"decision", "accept"}},
                    child)
                .value("ok")
                .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "completed", 6000);
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 2, 6000);
    }

    void jobApprovalSurvivesOwnerTurnEndAndIsNotAProviderRequest() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd, "owner", "ask"));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        QVERIFY(command(host, "session.start_turn", {{"prompt", "hang"}}).value("ok").toBool());
        QTRY_COMPARE(turns(log), 1);
        const auto start =
            command(host, "session.task.start", process(cwd, {"/usr/bin/printf", "approved"}));
        const auto id = start.value("task").toObject().value("task_id").toString();
        const auto pending = [&] {
            return command(host, "session.pending_approvals").value("pending_approvals").toArray();
        };
        QTRY_COMPARE(pending().size(), 1);
        const auto approval = pending().first().toObject().value("approval_id").toString();
        QCOMPARE(task(host, id).value("state"), "queued");
        StoredSession active;
        for (const auto& session : journal.sessions())
            if (session.id == "owner")
                active = session;
        emit adapter.notificationReceived(
            "turn/completed",
            {{"threadId", active.threadId},
             {"turn", QJsonObject{{"id", active.turnId}, {"status", "completed"}}}});
        QCOMPARE(pending().size(), 1);
        QCOMPARE(task(host, id).value("state"), "queued");
        const auto granted = command(
            host, "approval.resolve",
            {{"approval_id", approval}, {"resolution", QJsonObject{{"decision", "accept"}}}});
        QVERIFY2(granted.value("ok").toBool(),
                 qPrintable(granted.value("error_message").toString()));
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "completed", 5000);
        QCOMPARE(command(host, "session.task.output", {{"task_id", id}}).value("output"),
                 "approved");
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 2, 6000);
    }

    void processCompletionUsesDurableOwnerWakeAndNeverDuplicatesInference() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        const auto input = process(cwd, {"/usr/bin/printf", "hello"});
        const auto started = command(host, "session.task.start", input);
        QVERIFY2(started.value("ok").toBool(),
                 qPrintable(started.value("error_message").toString()));
        const auto id = started.value("task").toObject().value("task_id").toString();
        QVERIFY(!id.isEmpty());
        QVERIFY(started.value("accepted").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "completed", 5000);
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("completion").toObject().value("status"),
                                  "delivered", 6000);
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 1, 6000);
        QCOMPARE(command(host, "session.task.output", {{"task_id", id}}).value("output"), "hello");
        QCOMPARE(
            command(host, "session.task.start", input).value("task").toObject().value("task_id"),
            id);
        const auto event = task(host, id).value("completion_event_id").toString();
        QVERIFY(journal.backgroundTasks()
                    .acceptCompletion(event, {"owner", 1, {}})
                    .value("duplicate")
                    .toBool());
        QTest::qWait(1200);
        QCOMPARE(turns(log), 1);
        int messages = 0;
        for (const auto value : journal.eventsAfter("owner", 1, 0)) {
            const auto row = value.toObject();
            if (row.value("event_type") == "user.message" &&
                row.value("payload").toObject().value("background_task_event_id") == event)
                ++messages;
        }
        QCOMPARE(messages, 1);
    }

    void stoppedOwnerKeepsProcessAndHoldsWakeUntilExplicitUserTurn() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        QVERIFY(command(host, "session.start_turn", {{"prompt", "hang"}}).value("ok").toBool());
        QTRY_COMPARE(turns(log), 1);
        const auto start = command(host, "session.task.start", process(cwd, {"/bin/sleep", "2"}));
        const auto id = start.value("task").toObject().value("task_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "running", 4000);
        StoredSession active;
        for (const auto& session : journal.sessions())
            if (session.id == "owner")
                active = session;
        QVERIFY(command(host, "session.interrupt").value("ok").toBool());
        // FakeCodex acknowledges interrupts but deliberately leaves completion
        // notifications to the test, matching the existing lifecycle fixtures.
        emit adapter.notificationReceived(
            "turn/completed",
            {{"threadId", active.threadId},
             {"turn", QJsonObject{{"id", active.turnId}, {"status", "interrupted"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "completed", 6000);
        QTest::qWait(1200);
        QCOMPARE(turns(log), 1);
        QCOMPARE(task(host, id).value("completion").toObject().value("status"), "pending");
        QVERIFY(command(host, "session.start_turn", {{"prompt", "Resume fixture"}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 3, 6000);
        QCOMPARE(task(host, id).value("completion").toObject().value("status"), "delivered");
    }

    void agentTokenCannotChooseAnotherOwnerOrAcknowledgeCleanup() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        QVERIFY(seed(journal, cwd, "other"));
        CodexAdapter adapter(directory.filePath("never-start-provider"));
        SessionHost host(&journal, &adapter);
        const auto token = host.agentToken("owner");
        QVERIFY(!token.isEmpty());
        QJsonObject result;
        auto forged = process(cwd, {"/bin/sleep", "60"});
        forged.insert("owner_session_id", "other");
        host.handleAgentCommandDeferred(token, "session.task.start", forged,
                                        [&](const QJsonObject& value) { result = value; });
        QCOMPARE(result.value("error_code"), "invalid_request");
        host.handleAgentCommandDeferred(token, "session.task.resolve_cleanup",
                                        {{"task_id", "anything"}, {"revision", 1}},
                                        [&](const QJsonObject& value) { result = value; });
        QCOMPARE(result.value("error_code"), "forbidden");
        QCOMPARE(journal.backgroundTasks().nonterminal().value("tasks").toArray().size(), 0);
    }

    void failedChildCancelsDescendantsWithoutReceivingTheirWake() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        const auto started = command(host, "session.task.start",
                                     {{"client_request_id", "failed-child"},
                                      {"kind", "agent"},
                                      {"title", "Fail with descendants"},
                                      {"timeout_ms", 20000},
                                      {"execution_spec", QJsonObject{{"prompt", "hang"}}}});
        const auto id = started.value("task").toObject().value("task_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "running", 6000);
        const auto child = task(host, id)
                               .value("execution_identity")
                               .toObject()
                               .value("child_session_id")
                               .toString();
        const auto descendant =
            command(host, "session.task.start", process(cwd, {"/bin/sleep", "15"}), child)
                .value("task")
                .toObject()
                .value("task_id")
                .toString();
        QVERIFY(!descendant.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(journal.backgroundTasks().runtimeTask(descendant).value("state"),
                                  "running", 6000);
        StoredSession current;
        for (const auto& session : journal.sessions())
            if (session.id == child)
                current = session;
        emit adapter.notificationReceived(
            "error", {{"threadId", current.threadId},
                      {"turnId", current.turnId},
                      {"willRetry", false},
                      {"error", QJsonObject{{"message", "Fixture child failure"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(journal.backgroundTasks().runtimeTask(descendant).value("state"),
                                  "cancelled", 6000);
        QCOMPARE(task(host, id).value("state"), "stopping");
        QCOMPARE(turns(log), 1);
        // FakeCodex acknowledges interrupt without a completion event. Supply
        // that independent evidence before expecting terminal cleanup.
        emit adapter.notificationReceived(
            "turn/completed",
            {{"threadId", current.threadId},
             {"turn", QJsonObject{{"id", current.turnId}, {"status", "failed"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "failed", 7000);
        const auto cancelled = journal.backgroundTasks().runtimeTask(descendant);
        QCOMPARE(cancelled.value("state"), "cancelled");
        QCOMPARE(cancelled.value("completion").toObject().value("status"), "suppressed");
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 2, 6000); // Initial child, then owner result.
        QTest::qWait(800);
        QCOMPARE(turns(log), 2);
    }

    void managedChildUsesDedicatedLinkedSessionAndReturnsItsOutput() {
        QTemporaryDir directory;
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto log = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(seed(journal, cwd));
        CodexAdapter adapter(directory.filePath("fake-codex"));
        SessionHost host(&journal, &adapter);
        const auto cleanup = qScopeGuard([&] { adapter.stop(); });
        const auto started =
            command(host, "session.task.start",
                    {{"client_request_id", "agent-request"},
                     {"kind", "agent"},
                     {"title", "Managed fixture"},
                     {"timeout_ms", 10000},
                     {"execution_spec", QJsonObject{{"prompt", "Hello fixture"}}}});
        QVERIFY2(started.value("ok").toBool(),
                 qPrintable(started.value("error_message").toString()));
        const auto id = started.value("task").toObject().value("task_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(task(host, id).value("state"), "completed", 7000);
        const auto child = task(host, id)
                               .value("execution_identity")
                               .toObject()
                               .value("child_session_id")
                               .toString();
        QVERIFY(!child.isEmpty());
        QVERIFY(child != "owner");
        QVERIFY(journal.sessionExists(child));
        QCOMPARE(journal.backgroundTasks().linkedTask(child, 1).value("task").toObject().value(
                     "task_id"),
                 id);
        QCOMPARE(command(host, "session.task.output", {{"task_id", id}}).value("output"),
                 "finished");
        QTRY_COMPARE_WITH_TIMEOUT(turns(log), 2, 6000);
    }
};
QTEST_GUILESS_MAIN(ManagedTaskHostTest)
#include "ManagedTaskHostTest.moc"
