// T-39: an urgent message to a conversation whose turn cannot be steered now says so in its
// receipt, names what the receiver waits on, and is delivered at the first point it can be.
#include "agents/AgentBroker.h"
#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "AgentTestFixture.h"

#include <QJsonArray>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;
using namespace agent_test;

class AgentUrgentDeliveryTest final : public QObject {
    Q_OBJECT

  private slots:
    void urgentToABackgroundTaskSaysItCannotSteerYet() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        // The receiver's turn ended while a command it started keeps running: it is busy and
        // has no turn to steer.
        const QString receiver = fixture.start("claude", "background");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "tasks.changed"), 1, 5000);

        const QJsonObject plain = fixture.agents(
            sender, "agents.send", {{"to", fixture.number(receiver)}, {"text", "when you can"}});
        QCOMPARE(plain.value("delivery").toString(), "queued");
        QVERIFY(!plain.contains("waiting_on"));

        const QJsonObject urgent = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", "stop and check"}, {"urgent", true}});
        QCOMPARE(urgent.value("delivery").toString(), "urgent_waiting");
        QVERIFY(urgent.value("waiting_on").toString().contains("background task"));
        const QString note = urgent.value("note").toString();
        QVERIFY2(note.startsWith("Could not steer yet: conversation " + fixture.number(receiver) +
                                 " is waiting on a background task"),
                 qPrintable(note));
        QVERIFY(note.contains("do not send it again"));
        // Nothing was handed to the provider yet.
        QVERIFY(!logText(fixture.claudeLog).contains("steer:" + receiver.toUtf8()));
        QCOMPARE(fixture.journal->agentMessage(urgent.value("message_id").toString())->state,
                 "queued");

        // The command ends: the first point the provider allows starts a turn with both.
        auto answers = std::make_shared<QList<QJsonObject>>();
        fixture.host->handleCommandDeferred(
            request("session.task.stop", {{"task_id", "t1"}}, receiver, 1), "desktop",
            [answers](const QJsonObject& answer) { answers->append(answer); });
        QTRY_VERIFY_WITH_TIMEOUT(!answers->isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.journal->agentMessage(urgent.value("message_id").toString())->state,
            "delivered", 5000);
        QCOMPARE(fixture.journal->agentMessage(plain.value("message_id").toString())->state,
                 "delivered");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "user.message"), 2, 5000);
        const QJsonObject started = fixture.payloads(receiver, "user.message").last();
        QCOMPARE(started.value("origin").toString(), "agent");
        QVERIFY(started.value("text").toString().contains("stop and check"));
    }

    void urgentToASteerableTurnStillSteers() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const QString receiver = fixture.start("claude", "hang");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.started"), 1, 5000);
        const QJsonObject sent = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", "look now"}, {"urgent", true}});
        QCOMPARE(sent.value("delivery").toString(), "steering");
        QVERIFY(!sent.contains("waiting_on"));
        QVERIFY(!sent.contains("note"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.steered"), 1, 5000);
        QCOMPARE(fixture.journal->agentMessage(sent.value("message_id").toString())->delivery,
                 "steer");
    }

    void urgentToATurnInAToolCallSaysWhenItIsRead() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        // The receiver's turn runs a long command: the steer is handed over, but the
        // provider reads it only when the command ends.
        const QString receiver = fixture.start("claude", "tool-hang");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "item.started"), 1, 5000);
        const QJsonObject sent = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", "look now"}, {"urgent", true}});
        QCOMPARE(sent.value("delivery").toString(), "steering");
        QCOMPARE(sent.value("waiting_on").toString(), "a command (Bash)");
        QVERIFY2(sent.value("note").toString().contains("reads the message when that call ends"),
                 qPrintable(sent.value("note").toString()));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.steered"), 1, 5000);
    }

    void receiptStatesAreDescribedToAgents() {
        // The agents' instructions and the agents_send tool name each receipt state.
        const QString instructions = agent_messaging::toolInstructions("4821");
        for (const QString& state :
             {"starting", "queued", "steering", "urgent_waiting", "recorded"})
            QVERIFY2(instructions.contains(state), qPrintable(state));
        QVERIFY(instructions.contains("waiting_on"));
        const agent_mcp::DaemonCall call = [](const QString&, const QJsonObject&) {
            return QJsonObject{{"ok", true}};
        };
        const QJsonArray tools =
            agent_mcp::handleMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                     call)
                .value("result")
                .toObject()
                .value("tools")
                .toArray();
        QJsonObject send;
        for (const QJsonValue& tool : tools)
            if (tool.toObject().value("name").toString() == "agents_send")
                send = tool.toObject();
        const QString description = send.value("description").toString();
        for (const QString& state :
             {"starting", "queued", "steering", "urgent_waiting", "recorded"})
            QVERIFY2(description.contains(state), qPrintable(state));
        QVERIFY(send.value("inputSchema")
                    .toObject()
                    .value("properties")
                    .toObject()
                    .value("urgent")
                    .toObject()
                    .value("description")
                    .toString()
                    .contains("urgent_waiting"));
    }
};

QTEST_GUILESS_MAIN(AgentUrgentDeliveryTest)
#include "AgentUrgentDeliveryTest.moc"
