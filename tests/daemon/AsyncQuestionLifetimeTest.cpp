#include "FakeCodex.h"
#include "providers/CodexAdapter.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ketplus::daemon;
namespace {
QJsonObject command(const QString& type, const QString& session, const QJsonObject& payload = {}) {
    return {{"message_type", type},
            {"message_id", QUuid::createUuid().toString()},
            {"session_id", session},
            {"session_generation", 1},
            {"payload", payload}};
}
QJsonObject question(const QString& id = "async-question") {
    return {{"id", id},
            {"type", "agentMessage"},
            {"delivery", "async"},
            {"questions", QJsonArray{QJsonObject{{"title", "Which target?"},
                                                 {"options", QJsonArray{"iOS", "Android"}}}}}};
}
} // namespace
class AsyncQuestionLifetimeTest : public QObject {
    Q_OBJECT
  private slots:
    // #126: the expiry timer wakes the daemon only while a request can expire.
    void expiryTimerRunsOnlyWhileARequestCanExpire() {
        ApprovalBroker broker;
        QVERIFY(!broker.expiryTimerActive());
        const QJsonObject details{{"request_type", "command"}, {"command", "ls"}};
        QVERIFY(!broker.request("s", 1, 1, "approval", details).isEmpty());
        QVERIFY(!broker.expiryTimerActive()); // no expiry, nothing to tick for
        QSignalSpy expired(&broker, &ApprovalBroker::expired);
        QVERIFY(!broker.request("s", 1, 2, "approval", details, 100).isEmpty());
        QVERIFY(broker.expiryTimerActive());
        QTRY_COMPARE_WITH_TIMEOUT(expired.size(), 1, 2000);
        QVERIFY(!broker.expiryTimerActive());
        // Answered before it expired: the next tick stops the timer.
        const QString answered = broker.request("s", 1, 3, "approval", details, 60'000);
        QVERIFY(broker.expiryTimerActive());
        QVERIFY(broker.resolve(answered, "s", 1, "approval", {{"decision", "accept"}}).accepted);
        QTRY_VERIFY_WITH_TIMEOUT(!broker.expiryTimerActive(), 2000);
        QCOMPARE(expired.size(), 1);
    }
    void onlyAsyncQuestionsInTheOwningGenerationExpire() {
        ApprovalBroker broker;
        QSignalSpy expired(&broker, &ApprovalBroker::expired);
        QSignalSpy answers(&broker, &ApprovalBroker::providerResolutionReady);
        const QJsonObject details{
            {"is_blocking", false},
            {"questions",
             QJsonArray{QJsonObject{{"id", "q"},
                                    {"question", "Which target?"},
                                    {"options", QJsonArray{QJsonObject{{"label", "iOS"}}}}}}}};
        const QJsonObject tag{{"ketplus_async_question", true}};
        const auto active = broker.request("s", 1, tag, "input", details);
        QVERIFY(!active.isEmpty());
        QVERIFY(!broker.request("s", 2, tag, "input", details).isEmpty());
        QVERIFY(!broker.request("other", 1, tag, "input", details).isEmpty());
        QVERIFY(!broker.request("s", 1, 15, "input", details).isEmpty());
        broker.expireAssistantQuestions("s", 1);
        QCOMPARE(expired.size(), 1);
        QCOMPARE(expired.first().first().toString(), active);
        QCOMPARE(answers.size(), 0);
        QCOMPARE(broker.pendingPage("s", 1).value("pending_approvals").toArray().size(), 1);
        QVERIFY(!broker.resolve(active, "s", 1, "input", {{"answers", QJsonObject{}}}).accepted);
        broker.expireAssistantQuestions("s", 1);
        QCOMPARE(expired.size(), 1);
    }
    void terminalTurnClosesQuestionAndLateReplayCannotReopenIt() {
        QTemporaryDir dir;
        const auto executable = dir.filePath("codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("log")));
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto started = host.handleCommand(
            command("session.start_turn", {}, {{"worktree_path", dir.path()}, {"prompt", "hang"}}),
            "test");
        QVERIFY(started.value("ok").toBool());
        const auto session = started.value("session_id").toString();
        QTRY_VERIFY(!journal.sessions().isEmpty() && journal.sessions().first().state == "running");
        const auto thread = journal.sessions().first().threadId;
        const auto turn = journal.sessions().first().turnId;
        const auto item = question();
        emit adapter.notificationReceived("item/completed",
                                          {{"threadId", thread}, {"turnId", turn}, {"item", item}});
        auto pending = [&] {
            return host.handleCommand(command("session.pending_approvals", session), "test")
                .value("pending_approvals")
                .toArray();
        };
        QCOMPARE(pending().size(), 1);
        const auto approval = pending().first().toObject().value("approval_id");
        emit adapter.notificationReceived(
            "turn/completed",
            {{"threadId", thread}, {"turn", QJsonObject{{"id", turn}, {"status", "completed"}}}});
        QVERIFY(pending().isEmpty());
        bool receipt = false;
        for (const auto& row : journal.eventsAfter(session, 1, 0, 100)) {
            const auto event = row.toObject();
            if (event.value("event_type") == "question.expired" &&
                event.value("payload").toObject().value("approval_id") == approval)
                receipt = true;
        }
        QVERIFY(receipt);
        emit adapter.notificationReceived(
            "item/completed", {{"threadId", thread}, {"turnId", turn}, {"item", question("late")}});
        QVERIFY(pending().isEmpty());
        const auto next = host.handleCommand(
            command("session.start_turn", session, {{"prompt", "hang"}}), "test");
        QVERIFY(next.value("ok").toBool());
        QTRY_COMPARE(journal.sessions().first().currentTurnSequence, 2);
        emit adapter.notificationReceived(
            "item/completed",
            {{"threadId", thread}, {"turnId", turn}, {"item", question("old-replay")}});
        QVERIFY(pending().isEmpty());
        adapter.stop();
    }
};
QTEST_GUILESS_MAIN(AsyncQuestionLifetimeTest)
#include "AsyncQuestionLifetimeTest.moc"
