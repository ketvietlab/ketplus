// ADR 0027: an urgent message never steers a running turn. The receipt says queued, and the
// agent instructions and the tool description name no steering state.
#include "agents/AgentBroker.h"
#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "AgentTestFixture.h"

#include <QJsonArray>
#include <QtTest>

using namespace ketplus::daemon;
using namespace agent_test;

class AgentUrgentDeliveryTest final : public QObject {
    Q_OBJECT

  private slots:
    void urgentToABusyReceiverIsQueuedInEveryState_data() {
        QTest::addColumn<QString>("prompt");
        QTest::newRow("running turn") << "hang";
        QTest::newRow("tool call") << "tool-hang";
        QTest::newRow("wait loop") << "wait-loop-hang";
        QTest::newRow("background task") << "background";
    }

    void urgentToABusyReceiverIsQueuedInEveryState() {
        QFETCH(QString, prompt);
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const QString receiver = fixture.start("claude", prompt);
        if (prompt == "background")
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "tasks.changed"), 1, 5000);
        else
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.started"), 1, 5000);
        const QJsonObject sent = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", "look now"}, {"urgent", true}});
        QCOMPARE(sent.value("delivery").toString(), "queued");
        QVERIFY(!sent.contains("waiting_on"));
        QVERIFY(!sent.contains("note"));
        QTest::qWait(100);
        QVERIFY(!logText(fixture.claudeLog).contains("steer:" + receiver.toUtf8()));
        QCOMPARE(fixture.count(receiver, "turn.steered"), 0);
        QCOMPARE(fixture.journal->agentMessage(sent.value("message_id").toString())->state,
                 "queued");
        QVERIFY(fixture.journal->agentMessage(sent.value("message_id").toString())->urgent);
    }

    void receiptStatesAreDescribedToAgents() {
        const QString instructions = agent_messaging::toolInstructions();
        for (const QString& state : {"starting", "queued", "recorded"})
            QVERIFY2(instructions.contains(state), qPrintable(state));
        for (const QString& gone : {"steering", "urgent_waiting", "waiting_on"})
            QVERIFY2(!instructions.contains(gone), qPrintable(gone));
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
        for (const QString& state : {"starting", "queued", "recorded"})
            QVERIFY2(description.contains(state), qPrintable(state));
        for (const QString& gone : {"steering", "urgent_waiting", "waiting_on"})
            QVERIFY2(!description.contains(gone), qPrintable(gone));
        const QJsonObject urgent = send.value("inputSchema")
                                       .toObject()
                                       .value("properties")
                                       .toObject()
                                       .value("urgent")
                                       .toObject();
        QCOMPARE(urgent.value("type").toString(), "boolean");
        QVERIFY(!urgent.value("description").toString().contains("urgent_waiting"));
    }
};

QTEST_GUILESS_MAIN(AgentUrgentDeliveryTest)
#include "AgentUrgentDeliveryTest.moc"
