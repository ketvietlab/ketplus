#include "runtime/AwaitingReply.h"
#include "agents/AgentBroker.h"
#include "agents/AgentMcpServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "AgentTestFixture.h"

#include <QJsonArray>
#include <QStandardPaths>
#include <QtTest>

using namespace ketplus::daemon;
using namespace agent_test;

namespace {

bool awaiting(Fixture& fixture, const QString& id) {
    const QJsonObject listed = fixture.host->handleCommand(request("session.list"), "desktop-test");
    for (const QJsonValue& value : listed.value("sessions").toArray())
        if (value.toObject().value("session_id").toString() == id)
            return value.toObject().value("awaiting_reply").toBool();
    return false;
}

} // namespace

// #125 "Waiting for you": fake providers only, no provider quota.
class AwaitingReplyTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void theHeuristicReadsOnlyTheLastParagraph_data() {
        QTest::addColumn<QString>("answer");
        QTest::addColumn<bool>("asks");
        QTest::newRow("question") << "Done.\n\nShould I push and open the PR?" << true;
        QTest::newRow("bold question") << "Done.\n\n**Which option do you want?**" << true;
        QTest::newRow("full-width") << "完了。\n\nどちらにしますか？" << true;
        QTest::newRow("summary") << "Merged #483. All tests pass." << false;
        QTest::newRow("earlier question")
            << "Why did it fail? A stale cache.\n\nFixed it." << false;
        QTest::newRow("table last") << "Shall I?\n\n| Task | Status |\n|---|---|" << false;
        QTest::newRow("code last") << "Run this:\n\n```\nfoo?\n```" << false;
        QTest::newRow("empty") << "" << false;
    }
    void theHeuristicReadsOnlyTheLastParagraph() {
        QFETCH(QString, answer);
        QFETCH(bool, asks);
        QCOMPARE(answerAsksTheUser(answer), asks);
    }

    void aQuestionEndsAsWaitingUntilTheUserActs() {
        Fixture fixture;
        QVERIFY(fixture.open());
        // A plain summary asks nothing.
        const QString id = fixture.start("claude", "hello");
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 1, 5000);
        QVERIFY(!fixture.payloads(id, "turn.completed").first().contains("awaiting_reply"));
        QVERIFY(!awaiting(fixture, id));

        // The heuristic: the answer's last paragraph asks the user.
        QVERIFY(!fixture.start("claude", "ask-approval", id).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 2, 5000);
        QVERIFY(fixture.payloads(id, "turn.completed").last().value("awaiting_reply").toBool());
        QVERIFY(awaiting(fixture, id));
        // Persisted across a restart.
        fixture.restart();
        QVERIFY(awaiting(fixture, id));

        // Mark as done clears it, and it stays cleared.
        const QJsonObject done =
            fixture.host->handleCommand(request("session.mark_done", {}, id, 1), "desktop-test");
        QVERIFY2(done.value("ok").toBool(), qPrintable(text(done)));
        QVERIFY(!awaiting(fixture, id));
        fixture.restart();
        QVERIFY(!awaiting(fixture, id));

        // Writing again clears it as soon as the message is accepted.
        QVERIFY(!fixture.start("claude", "ask-approval", id).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 3, 5000);
        QVERIFY(awaiting(fixture, id));
        QVERIFY(!fixture.start("claude", "hang", id).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!awaiting(fixture, id), 5000);
    }

    void theExplicitSignalMarksAPlainAnswer() {
        Fixture fixture;
        QVERIFY(fixture.open());
        // The turn waits on a permission, so the agent can call the tool before it ends.
        const QString id = fixture.start("claude", "needs-approval");
        QVERIFY(!id.isEmpty());
        const auto pending = [&] {
            return fixture.host
                ->handleCommand(request("session.pending_approvals", {}, id, 1), "desktop-test")
                .value("pending_approvals")
                .toArray();
        };
        QTRY_COMPARE_WITH_TIMEOUT(pending().size(), qsizetype(1), 5000);
        QVERIFY(!fixture.agents(id, "agents.request_user_decision", {}).value("ok").toBool());
        const QJsonObject asked =
            fixture.agents(id, "agents.request_user_decision", {{"summary", "Approve the logo"}});
        QVERIFY2(asked.value("ok").toBool(), qPrintable(text(asked)));
        const QJsonObject resolved = fixture.host->handleCommand(
            request("approval.resolve",
                    {{"approval_id", pending().first().toObject().value("approval_id")},
                     {"resolution", QJsonObject{{"decision", "accept"}}}},
                    id, 1),
            "desktop-test");
        QVERIFY2(resolved.value("ok").toBool(), qPrintable(text(resolved)));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 1, 5000);
        // "finished" asks nothing; the signal alone makes the turn wait.
        QVERIFY(fixture.payloads(id, "turn.completed").first().value("awaiting_reply").toBool());
        QVERIFY(awaiting(fixture, id));
        // The signal belongs to that turn only.
        QVERIFY(!fixture.start("claude", "hello", id).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 2, 5000);
        QVERIFY(!awaiting(fixture, id));
        // Outside a turn there is nothing to mark.
        QVERIFY(!fixture.agents(id, "agents.request_user_decision", {{"summary", "Pick"}})
                     .value("ok")
                     .toBool());
    }

    void theToolIsListedAndRouted() {
        QStringList operations;
        const agent_mcp::DaemonCall daemon = [&](const QString& operation, const QJsonObject&) {
            operations.append(operation);
            return QJsonObject{{"ok", true}};
        };
        const QJsonArray tools =
            agent_mcp::handleMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                     daemon)
                .value("result")
                .toObject()
                .value("tools")
                .toArray();
        bool listed = false;
        for (const QJsonValue& value : tools)
            listed = listed || value.toObject().value("name") == "request_user_decision";
        QVERIFY(listed);
        const QJsonObject answer = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"},
             {"id", 2},
             {"method", "tools/call"},
             {"params", QJsonObject{{"name", "request_user_decision"},
                                    {"arguments", QJsonObject{{"summary", "Approve"}}}}}},
            daemon);
        QVERIFY(!answer.value("result").toObject().value("isError").toBool());
        // Listing asks who the agent is, for the team tools (ADR 0112); the call is routed.
        QCOMPARE(operations, (QStringList{"agents.whoami", "agents.request_user_decision"}));
        QVERIFY(AgentBroker::isOperation("agents.request_user_decision"));
    }
};

QTEST_GUILESS_MAIN(AwaitingReplyTest)
#include "AwaitingReplyTest.moc"
