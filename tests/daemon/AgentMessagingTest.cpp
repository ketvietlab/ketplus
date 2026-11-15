#include "agents/AgentBroker.h"
#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/SessionHost.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"

#include "AgentTestFixture.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

using namespace agent_test;

class AgentMessagingTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void deliveryGuidanceFollowsExpectReply() {
        using ketplus::daemon::agent_messaging::automaticTurnGuidance;
        using ketplus::daemon::agent_messaging::deliveryText;
        ketplus::daemon::AgentMessageRecord reply;
        reply.id = "m2";
        reply.kind = "reply";
        reply.replyTo = "m1";
        reply.fromNumber = "6212";
        reply.body = "Done.";
        const QString plainReply = deliveryText({reply});
        QVERIFY(plainReply.contains("This is a reply to your message m1."));
        QVERIFY(plainReply.contains("Your final message is not sent back"));
        QVERIFY(automaticTurnGuidance({QJsonObject{{"expect_reply", false}}})
                    .contains("KetPlus does not forward your final message"));

        // A reply that asks something back is forwarded, so it must not say otherwise.
        reply.expectReply = true;
        const QString answeredReply = deliveryText({reply});
        QVERIFY(answeredReply.contains("This is a reply to your message m1."));
        QVERIFY(answeredReply.contains("The sender expects a reply."));
        QVERIFY(!answeredReply.contains("not sent back"));
        QVERIFY(automaticTurnGuidance({QJsonObject{{"expect_reply", true}}})
                    .contains("KetPlus forwards it as the reply"));
    }

    void numbersAreHostUniqueStableAndListed() {
        Fixture fixture;
        QVERIFY(fixture.open());
        QStringList sessions;
        for (const QString& prompt : {"one", "two", "three"}) {
            const QString id = fixture.start("claude", prompt);
            QVERIFY(!id.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 1, 5000);
            sessions.append(id);
        }
        QSet<QString> numbers;
        static const QRegularExpression fourDigits("^[1-9][0-9]{3}$");
        for (const QString& id : sessions) {
            const QString number = fixture.number(id);
            QVERIFY2(fourDigits.match(number).hasMatch(), qPrintable(number));
            numbers.insert(number);
            const auto owner = fixture.journal->sessionForConversationNumber(number);
            QVERIFY(owner.has_value());
            QCOMPARE(owner->first, id);
        }
        QCOMPARE(numbers.size(), 3);
        const QJsonObject listed = fixture.host->handleCommand(request("session.list"), "desktop");
        for (const QJsonValue& value : listed.value("sessions").toArray())
            QCOMPARE(value.toObject().value("number").toString(),
                     fixture.number(value.toObject().value("session_id").toString()));
        const QString before = fixture.number(sessions.first());
        fixture.restart();
        QCOMPARE(fixture.number(sessions.first()), before);

        // A journal written before numbers existed is backfilled when the daemon opens it.
        StoredSession legacy;
        legacy.id = "legacy-session";
        legacy.worktreePath = fixture.directory.path();
        legacy.adapter = "codex";
        legacy.state = "ready";
        legacy.title = "Before numbers";
        QVERIFY(fixture.journal->upsertSession(legacy));
        QVERIFY(fixture.number(legacy.id).isEmpty());
        fixture.restart();
        QVERIFY(fourDigits.match(fixture.number(legacy.id)).hasMatch());
        QVERIFY(!numbers.contains(fixture.number(legacy.id)));
        QVERIFY(protocol::features(1).contains("agents.messaging/1"));
    }

    void sentNoticeShowsABoundedExcerpt() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        const QJsonObject sent = fixture.agents(
            sender, "agents.send",
            {{"to", "#" + fixture.number(receiver)}, {"text", QString(2000, QLatin1Char('x'))}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(text(sent)));
        QCOMPARE(fixture.payloads(sender, "agent_message.state").last().value("text").toString(),
                 QString(500, QLatin1Char('x')) + QStringLiteral("…"));
    }

    void messageStartsIdleTurnAndFinalAnswerReturnsAsReply() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);

        const QJsonObject whoami = fixture.agents(sender, "agents.whoami");
        QCOMPARE(whoami.value("number").toString(), fixture.number(sender));
        const QJsonObject list = fixture.agents(sender, "agents.list");
        QCOMPARE(list.value("total").toInt(), 2);

        const QJsonObject sent = fixture.agents(sender, "agents.send",
                                                {{"to", "#" + fixture.number(receiver)},
                                                 {"text", "Please review the parser."},
                                                 {"expect_reply", true}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(text(sent)));
        QCOMPARE(sent.value("delivery").toString(), "starting");
        QCOMPARE(sent.value("to").toObject().value("number").toString(), fixture.number(receiver));
        // The sender's transcript names the receiver, whatever its provider's tool row says.
        const QJsonObject sentNotice = fixture.payloads(sender, "agent_message.state").last();
        QCOMPARE(sentNotice.value("state").toString(), "sent");
        QCOMPARE(sentNotice.value("delivery").toString(), "starting");
        QCOMPARE(sentNotice.value("peer_number").toString(), fixture.number(receiver));
        QCOMPARE(sentNotice.value("peer_title").toString(), "receiver");
        QVERIFY(sentNotice.value("expect_reply").toBool());
        // The sender's note shows what it sent, bounded so a 32 KiB message stays small.
        QCOMPARE(sentNotice.value("text").toString(), "Please review the parser.");

        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 2, 5000);
        const QJsonObject delivered = fixture.payloads(receiver, "user.message").last();
        QCOMPARE(delivered.value("origin").toString(), "agent");
        const QJsonObject message = delivered.value("agent_messages").toArray().first().toObject();
        QCOMPARE(message.value("from_number").toString(), fixture.number(sender));
        QCOMPARE(message.value("kind").toString(), "request");
        QCOMPARE(delivered.value("text").toString(), "Please review the parser.");
        const QByteArray providerLog = logText(fixture.claudeLog);
        QVERIFY(providerLog.contains("[KetPlus agent message]"));
        QVERIFY(providerLog.contains("<ketplus_agent_turn>"));
        // A request's final message is forwarded, so the receiver is told to answer with it.
        QVERIFY(providerLog.contains("KetPlus forwards it as the reply to the request."));

        // The receiver's final answer comes back to the sender, which starts its own turn.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 2, 5000);
        const QJsonObject reply = fixture.payloads(sender, "user.message").last();
        QCOMPARE(reply.value("origin").toString(), "agent");
        const QJsonObject replyMessage = reply.value("agent_messages").toArray().first().toObject();
        QCOMPARE(replyMessage.value("kind").toString(), "reply");
        QCOMPARE(replyMessage.value("body").toString(), "finished");
        QCOMPARE(replyMessage.value("reply_to").toString(), sent.value("message_id").toString());
        QCOMPARE(fixture.journal->agentMessage(sent.value("message_id").toString())->state,
                 "answered");
        // The receiver is told its final answer went back as the reply.
        const QJsonObject replied = fixture.payloads(receiver, "agent_message.state").last();
        QCOMPARE(replied.value("state").toString(), "replied");
        QCOMPARE(replied.value("peer_number").toString(), fixture.number(sender));
        QCOMPARE(replied.value("reply_to").toString(), sent.value("message_id").toString());
        // The reply's turn is told its final message stays local and to use agents_send.
        const QByteArray replyLog = logText(fixture.claudeLog);
        QVERIFY(replyLog.contains("KetPlus does not forward your final message"));
        QVERIFY(replyLog.contains(QStringLiteral("call agents_send with reply_to=\\\"%1\\\"")
                                      .arg(replyMessage.value("message_id").toString())
                                      .toUtf8()));
        // A reply that asks nothing ends the exchange.
        QTest::qWait(300);
        QCOMPARE(fixture.count(receiver, "turn.completed"), 2);
        QCOMPARE(fixture.count(sender, "turn.completed"), 2);
        // The title stays the user's.
        QVERIFY(!fixture.host->handleCommand(request("session.list"), "d").isEmpty());
        const QJsonObject inbox = fixture.agents(sender, "agents.inbox");
        QCOMPARE(inbox.value("messages").toArray().size(), 2);
    }

    // A runtime.error inside a turn that goes on ends nothing, also when the journal takes
    // it late and the conversation is idle by then: the answer still reaches the sender.
    void anErrorInsideATurnDoesNotFailItsRequest_data() {
        QTest::addColumn<bool>("journaledLate");
        QTest::newRow("at once") << false;
        QTest::newRow("journaled late") << true;
    }

    void anErrorInsideATurnDoesNotFailItsRequest() {
        QFETCH(bool, journaledLate);
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("codex", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        const QJsonObject sent = fixture.agents(sender, "agents.send",
                                                {{"to", "#" + fixture.number(receiver)},
                                                 {"text", "please hang"},
                                                 {"expect_reply", true}});
        QVERIFY2(sent.value("ok").toBool(), qPrintable(text(sent)));
        const QString id = sent.value("message_id").toString();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.journal->agentMessage(id)->state,
                                  QStringLiteral("delivered"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(logText(fixture.claudeLog).contains("please hang"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.started"), 2, 5000);
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", "late-error");
            database.setDatabaseName(fixture.directory.filePath("journal.sqlite3"));
            QVERIFY(database.open());
            QSqlQuery query(database);
            if (journaledLate)
                QVERIFY(query.exec(
                    "CREATE TRIGGER late_error BEFORE INSERT ON events WHEN NEW.event_type = "
                    "'runtime.error' BEGIN SELECT RAISE(ABORT, 'not now'); END"));
            fixture.claude->canonicalEvent(receiver, "runtime.error",
                                           {{"message", "a tool failed; the turn goes on"}});
            QCOMPARE(fixture.journal->agentMessage(id)->state, QStringLiteral("delivered"));
            fixture.claude->canonicalEvent(receiver, "item.completed",
                                           {{"item_id", "late-answer"},
                                            {"item_type", "agentMessage"},
                                            {"text", "Recovered answer"}});
            fixture.claude->canonicalEvent(receiver, "turn.completed", {{"status", "completed"}});
            if (journaledLate)
                QVERIFY(query.exec("DROP TRIGGER late_error"));
            database.close();
        }
        QSqlDatabase::removeDatabase("late-error");
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.journal->agentMessage(id)->state != QStringLiteral("delivered"), 5000);
        QCOMPARE(fixture.journal->agentMessage(id)->state, QStringLiteral("answered"));
    }

    void busyReceiverGetsMessagesWhenItsTurnEnds() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const QString receiver = fixture.start("claude", "slow-start");
        QVERIFY(!receiver.isEmpty());
        const QJsonObject sent = fixture.agents(
            sender, "agents.send", {{"to", fixture.number(receiver)}, {"text", "first"}});
        QCOMPARE(sent.value("delivery").toString(), "queued");
        const QJsonObject second = fixture.agents(
            sender, "agents.send", {{"to", fixture.number(receiver)}, {"text", "second"}});
        QVERIFY(second.value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 2, 5000);
        // Both queued messages arrive together, in order, in one automatic turn.
        const QJsonObject delivered = fixture.payloads(receiver, "user.message").last();
        const QJsonArray messages = delivered.value("agent_messages").toArray();
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages.at(0).toObject().value("body").toString(), "first");
        QCOMPARE(messages.at(1).toObject().value("body").toString(), "second");
        QCOMPARE(fixture.journal->agentMessage(sent.value("message_id").toString())->delivery,
                 "auto");
    }

    void queuedMessagesAreBoundedPerSenderAndTurn() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const QString receiver = fixture.start("claude", "slow-start");
        QVERIFY(!receiver.isEmpty());
        const QString other = fixture.start("claude", "other");
        QVERIFY(!other.isEmpty());
        const auto send = [&](const QString& from, const int index) {
            return fixture.agents(from, "agents.send",
                                  {{"to", fixture.number(receiver)},
                                   {"text", QString::number(index) + QString(20 * 1024, 'x')}});
        };
        // A sender looping on a busy receiver is stopped once it has a full backlog there.
        for (int index = 0; index < AgentBroker::maximumUndeliveredPerSender; ++index)
            QVERIFY(send(sender, index).value("ok").toBool());
        QCOMPARE(send(sender, 99).value("error_code").toString(), "receiver_backlog");
        // The backlog is the sender's own: another conversation can still reach the receiver.
        QVERIFY(send(other, 100).value("ok").toBool());

        // Each turn takes only what fits its input; the rest arrive in the turns that follow.
        // The fake provider echoes each large prompt as many events: read every page.
        const auto userMessages = [&] {
            QList<QJsonObject> result;
            for (qint64 cursor = 0;;) {
                const QJsonArray page = fixture.journal->eventsAfter(receiver, 1, cursor);
                if (page.isEmpty())
                    return result;
                for (const QJsonValue& value : page)
                    if (value.toObject().value("event_type").toString() == "user.message")
                        result.append(value.toObject().value("payload").toObject());
                cursor = page.last().toObject().value("cursor").toInteger();
            }
        };
        const auto delivered = [&] {
            qsizetype total = 0;
            for (const QJsonObject& turn : userMessages())
                total += turn.value("agent_messages").toArray().size();
            return total;
        };
        QTRY_COMPARE_WITH_TIMEOUT(delivered(), AgentBroker::maximumUndeliveredPerSender + 1, 20000);
        const QList<QJsonObject> turns = userMessages();
        QVERIFY(turns.size() >= 4);
        for (const QJsonObject& turn : turns) {
            qsizetype bytes = 0;
            for (const QJsonValue& message : turn.value("agent_messages").toArray())
                bytes += message.toObject().value("body").toString().toUtf8().size();
            QVERIFY2(bytes <= AgentBroker::maximumDeliveryBytes,
                     qPrintable(QString::number(bytes)));
        }
        QVERIFY(fixture.journal->agentMessagesTo(receiver, 1, {"failed"}).isEmpty());
        QVERIFY(send(sender, 101).value("ok").toBool());
    }

    // Escaping counts: 16 KiB of control characters serializes to about 192 KiB in user.message.
    void encodedSizeDecidesWhatIsAccepted() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        // Within 32 KiB of text but too large once encoded: refused, never accepted and failed.
        const QJsonObject refused = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", QString(32 * 1024, QChar(1))}});
        QCOMPARE(refused.value("error_code").toString(), "message_too_large");
        QVERIFY(fixture.journal->agentMessagesTo(receiver, 1, {"queued", "failed"}).isEmpty());
        const QJsonObject sent = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", QString(16 * 1024, QChar(1))}});
        QVERIFY(sent.value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(
            fixture.journal->agentMessagesTo(receiver, 1, {"delivered"}).size(), 1, 5000);
        QVERIFY(fixture.journal->agentMessagesTo(receiver, 1, {"failed"}).isEmpty());
    }

    // The sender's title is in every message's header and entry, so it counts too.
    void senderTitleCountsTowardsTheTurnBudget() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QVERIFY(fixture.host
                    ->handleCommand(
                        request("session.rename", {{"title", QString(4096, QChar(1))}}, sender, 1),
                        "desktop-test")
                    .value("ok")
                    .toBool());
        const QString receiver = fixture.start("claude", "slow-start");
        QVERIFY(!receiver.isEmpty());
        for (int index = 0; index < AgentBroker::maximumUndeliveredPerSender; ++index)
            QVERIFY(fixture
                        .agents(sender, "agents.send",
                                {{"to", fixture.number(receiver)}, {"text", QString(4096, 'x')}})
                        .value("ok")
                        .toBool());
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.journal->agentMessagesTo(receiver, 1, {"queued", "held"}).isEmpty(), 20000);
        QCOMPARE(fixture.journal->agentMessagesTo(receiver, 1, {"delivered"}).size(),
                 AgentBroker::maximumUndeliveredPerSender);
        QVERIFY(fixture.journal->agentMessagesTo(receiver, 1, {"failed"}).isEmpty());
    }

    // A conversation another conversation woke names its sender while the turn runs.
    void automaticTurnsNameTheirSenderWhileTheyRun() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        QVERIFY(fixture.host->agentTurnSenders().isEmpty());
        const QJsonObject sent = fixture.agents(
            sender, "agents.send", {{"to", fixture.number(receiver)}, {"text", "please hang"}});
        QCOMPARE(sent.value("delivery").toString(), "starting");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.started"), 2, 5000);
        const QJsonObject running = fixture.host->agentTurnSenders().value(receiver);
        QCOMPARE(running.value("number").toString(), fixture.number(sender));
        QCOMPARE(running.value("title").toString(), "sender");
        QCOMPARE(running.value("kind").toString(), "notice");
        QJsonObject listed;
        for (const QJsonValue& value :
             fixture.host->handleCommand(request("session.list"), "d").value("sessions").toArray())
            if (value.toObject().value("session_id").toString() == receiver)
                listed = value.toObject();
        QCOMPARE(listed.value("agent_turn").toObject(), running);
        QVERIFY(!fixture.host->agentTurnSenders().contains(sender));
    }

    void urgentMessageSteersTheRunningTurn() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const QString receiver = fixture.start("claude", "hang");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.started"), 1, 5000);
        const QJsonObject sent = fixture.agents(
            sender, "agents.send",
            {{"to", fixture.number(receiver)}, {"text", "stop and check"}, {"urgent", true}});
        QCOMPARE(sent.value("delivery").toString(), "steering");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.steered"), 1, 5000);
        const QJsonObject steered = fixture.payloads(receiver, "turn.steered").first();
        QCOMPARE(steered.value("origin").toString(), "agent");
        QCOMPARE(steered.value("text").toString(), "stop and check");
        QVERIFY(logText(fixture.claudeLog).contains("steer:" + receiver.toUtf8()));
        QCOMPARE(fixture.journal->agentMessage(sent.value("message_id").toString())->delivery,
                 "steer");
        // The running turn was steered, not replaced by an automatic one.
        QCOMPARE(fixture.count(receiver, "user.message"), 1);
    }

    void heldReleaseWaitsForBusyReceiverAndValidatesTheWholeBatch() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const auto sender = fixture.start("claude", "sender");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const auto receiver = fixture.start("claude", "slow-start");
        AgentMessageRecord held;
        held.id = "busy-held";
        held.chainId = "chain";
        held.hop = 7;
        held.fromSession = sender;
        held.fromGeneration = 1;
        held.fromNumber = fixture.number(sender);
        held.toSession = receiver;
        held.toGeneration = 1;
        held.toNumber = fixture.number(receiver);
        held.kind = "notice";
        held.body = "Release after this turn";
        held.state = "held";
        held.urgent = true; // explicit release never steers/interferes with the active turn
        held.createdAt = protocol::timestamp();
        QVERIFY(fixture.journal->insertAgentMessage(held));
        auto expired = held;
        expired.id = "expired-held";
        expired.expiresAt =
            QDateTime::currentDateTimeUtc().addSecs(-10).toString(Qt::ISODateWithMs);
        QVERIFY(fixture.journal->insertAgentMessage(expired));
        auto release = [&](QJsonArray ids) {
            return fixture.host->handleCommand(
                request("session.agent_messages.release", {{"message_ids", ids}}, receiver, 1),
                "desktop");
        };
        QVERIFY(!release({held.id, expired.id}).value("ok").toBool());
        QCOMPARE(fixture.journal->agentMessage(held.id)->state, "held");
        QVERIFY(!release({held.id, held.id}).value("ok").toBool());
        QVERIFY(!release({held.id, "missing"}).value("ok").toBool());
        const auto accepted = release({held.id});
        QVERIFY2(accepted.value("ok").toBool(), qPrintable(text(accepted)));
        QCOMPARE(fixture.journal->agentMessage(held.id)->state, "queued");
        QCOMPARE(fixture.count(receiver, "user.message"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 2, 5000);
        QCOMPARE(fixture.journal->agentMessage(held.id)->state, "delivered");
        QVERIFY(fixture.journal->agentMessage(held.id)->delivery.startsWith("user_authorized:"));
        QVERIFY(!logText(fixture.claudeLog).contains("steer:" + receiver.toUtf8()));
        QVERIFY(fixture.host->handleCommand(request("session.delete", {}, receiver, 1), "desktop")
                    .value("ok")
                    .toBool());
        QVERIFY(!release({expired.id}).value("ok").toBool());
    }

    void userReleasesOnlyTheSelectedHeldBatch_data() {
        QTest::addColumn<QString>("reason");
        for (const auto* reason : {"hop_limit", "chain_limit", "hourly_limit"})
            QTest::newRow(reason) << QString::fromLatin1(reason);
    }

    void userReleasesOnlyTheSelectedHeldBatch() {
        QFETCH(QString, reason);
        Fixture fixture;
        QVERIFY(fixture.open());
        const auto sender = fixture.start("claude", "sender");
        const auto receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        AgentMessageRecord message;
        message.id = "selected";
        message.chainId = "held-chain";
        message.hop = AgentBroker::maximumAutomaticHop + 1;
        message.fromSession = sender;
        message.fromGeneration = 1;
        message.fromNumber = fixture.number(sender);
        message.fromTitle = "sender";
        message.toSession = receiver;
        message.toGeneration = 1;
        message.toNumber = fixture.number(receiver);
        message.kind = "notice";
        message.body = "Review findings";
        message.state = "held";
        message.error = reason;
        message.createdAt = protocol::timestamp();
        QVERIFY(fixture.journal->insertAgentMessage(message));
        message.id = "not-selected";
        QVERIFY(fixture.journal->insertAgentMessage(message));
        const auto command = request("session.agent_messages.release",
                                     {{"message_ids", QJsonArray{"selected"}}}, receiver, 1);
        QVERIFY(!fixture.host->handleCommand(command, "cloud-control-v2").value("ok").toBool());
        QVERIFY(!fixture
                     .agents(sender, "session.agent_messages.release",
                             {{"message_ids", QJsonArray{"selected"}}})
                     .value("ok")
                     .toBool());
        const auto first = fixture.host->handleCommand(command, "desktop-test");
        QVERIFY2(first.value("ok").toBool(), qPrintable(text(first)));
        QCOMPARE(fixture.host->handleCommand(command, "desktop-test"), first);
        auto stale = command;
        stale.insert("session_generation", 2);
        QVERIFY(!fixture.host->handleCommand(stale, "desktop-test").value("ok").toBool());
        fixture.restart(); // authorization survived before the delivery timer ran
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 2, 5000);
        QCOMPARE(fixture.journal->agentMessage("selected")->state, "delivered");
        QCOMPARE(fixture.journal->agentMessage("not-selected")->state, "held");
        const auto origin = fixture.payloads(receiver, "user.message").last();
        QCOMPARE(origin.value("user_release_id"), command.value("message_id"));
        QCOMPARE(origin.value("agent_messages").toArray().size(), 1);
        QVERIFY(
            !fixture.host
                 ->handleCommand(request("session.agent_messages.release",
                                         {{"message_ids", QJsonArray{"selected"}}}, receiver, 1),
                                 "desktop-test")
                 .value("ok")
                 .toBool());
        fixture.restart();
        QTest::qWait(30);
        QCOMPARE(fixture.count(receiver, "user.message"), 2);
        // An unrelated later message still goes through the ordinary loop guard.
        message.id = "future";
        message.state = "queued";
        message.error.clear();
        QVERIFY(fixture.journal->insertAgentMessage(message));
        fixture.restart();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.journal->agentMessage("future")->state, QString("held"),
                                  5000);
    }

    void interruptedReleaseHasNoDuplicateTurnAndRemainsRetryable_data() {
        QTest::addColumn<bool>("journaled");
        QTest::newRow("before-input") << false;
        QTest::newRow("after-input") << true;
    }

    void interruptedReleaseHasNoDuplicateTurnAndRemainsRetryable() {
        QFETCH(bool, journaled);
        Fixture fixture;
        QVERIFY(fixture.open());
        const auto sender = fixture.start("claude", "sender");
        const auto receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        AgentMessageRecord message;
        message.id = "recover";
        message.chainId = "chain";
        message.hop = 7;
        message.fromSession = sender;
        message.fromGeneration = 1;
        message.fromNumber = fixture.number(sender);
        message.toSession = receiver;
        message.toGeneration = 1;
        message.toNumber = fixture.number(receiver);
        message.kind = "notice";
        message.body = "Results";
        message.state = "held";
        message.createdAt = protocol::timestamp();
        QVERIFY(fixture.journal->insertAgentMessage(message));
        QVERIFY(fixture.journal->authorizeAgentMessages(receiver, 1, {"recover"}, "release-crash"));
        message = *fixture.journal->agentMessage("recover");
        QVERIFY(fixture.journal->claimAgentRelease({message}, 2));
        if (journaled)
            QVERIFY(fixture.journal->appendEvent(
                        receiver, 1, "user.message",
                        {{"origin", "agent"},
                         {"user_release_id", "release-crash"},
                         {"agent_messages", QJsonArray{QJsonObject{{"message_id", "recover"}}}}}) >
                    0);
        fixture.restart();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.journal->agentMessage("recover")->state,
                                  journaled ? QString("delivered") : QString("failed"), 5000);
        QCOMPARE(fixture.count(receiver, "user.message"), journaled ? 2 : 1);
        if (!journaled) {
            const auto result = fixture.host->handleCommand(
                request("session.agent_messages.release", {{"message_ids", QJsonArray{"recover"}}},
                        receiver, 1),
                "desktop-test");
            QVERIFY2(result.value("ok").toBool(), qPrintable(text(result)));
            QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 2, 5000);
        }
    }

    void loopGuardHoldsMessagesForTheUsersNextTurn() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        AgentMessageRecord deep;
        deep.id = "deep-message";
        deep.chainId = "chain";
        deep.hop = AgentBroker::maximumAutomaticHop + 1;
        deep.fromSession = sender;
        deep.fromGeneration = 1;
        deep.fromNumber = fixture.number(sender);
        deep.fromTitle = "sender";
        deep.toSession = receiver;
        deep.toGeneration = 1;
        deep.toNumber = fixture.number(receiver);
        deep.kind = "notice";
        deep.body = "loop";
        deep.state = "queued";
        deep.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QVERIFY(fixture.journal->insertAgentMessage(deep));
        // A restarted daemon resumes queued delivery; the guard holds this one.
        fixture.restart();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.journal->agentMessage("deep-message")->state,
                                  QStringLiteral("held"), 5000);
        QCOMPARE(fixture.journal->agentMessage("deep-message")->error, "hop_limit");
        QCOMPARE(fixture.count(receiver, "turn.completed"), 1);
        QCOMPARE(fixture.payloads(receiver, "agent_message.state").last().value("state").toString(),
                 "held");
        QCOMPARE(
            fixture.payloads(sender, "agent_message.state").last().value("direction").toString(),
            "outgoing");

        // The user's next turn carries the held message and resolves `#NNNN` references.
        const QString number = fixture.number(sender);
        QCOMPARE(fixture.start("claude", "ask #" + number + " about it", receiver), receiver);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 2, 5000);
        const QByteArray providerLog = logText(fixture.claudeLog);
        QVERIFY(providerLog.contains("[KetPlus agent message]"));
        QVERIFY(providerLog.contains("[KetPlus conversation references]"));
        QVERIFY(providerLog.contains(("- #" + number + ": \"").toUtf8()));
        // `@agent/NNNN`, as the header shows a conversation, resolves the same way.
        QCOMPARE(fixture.start("claude", "ask @agent/" + number + " again", receiver), receiver);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 3, 5000);
        QCOMPARE(logText(fixture.claudeLog).count(("- #" + number + ": \"").toUtf8()), 2);
        const auto held = fixture.journal->agentMessage("deep-message");
        QCOMPARE(held->state, "delivered");
        QCOMPARE(held->delivery, "user_turn");
        QCOMPARE(fixture.payloads(receiver, "user.message").last().value("origin").toString(),
                 QString{});
    }

    void chainHourlyAndExpiryLimitsApply() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        const QString other = fixture.start("claude", "other");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(other, "turn.completed"), 1, 5000);
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        int serial = 0;
        const auto message = [&](const QString& to, const QString& chain, const QString& state) {
            AgentMessageRecord record;
            record.id = QStringLiteral("m%1").arg(++serial);
            record.chainId = chain;
            record.hop = 1;
            record.fromSession = sender;
            record.fromGeneration = 1;
            record.fromNumber = fixture.number(sender);
            record.toSession = to;
            record.toGeneration = 1;
            record.toNumber = fixture.number(to);
            record.kind = "notice";
            record.body = "x";
            record.state = state;
            record.createdAt = now;
            if (state == "delivered") {
                record.delivery = "auto";
                record.deliveredTurn = serial;
                record.deliveredAt = now;
            }
            return record;
        };
        // A chain that already started its maximum number of automatic turns elsewhere.
        for (int turn = 0; turn < AgentBroker::maximumChainTurns; ++turn)
            QVERIFY(fixture.journal->insertAgentMessage(message(other, "busy-chain", "delivered")));
        AgentMessageRecord chained = message(receiver, "busy-chain", "queued");
        QVERIFY(fixture.journal->insertAgentMessage(chained));
        // A receiver that already ran its hourly maximum of automatic turns.
        for (int turn = 0; turn < AgentBroker::maximumHourlyTurns; ++turn)
            QVERIFY(fixture.journal->insertAgentMessage(
                message(other, QStringLiteral("chain-%1").arg(turn), "delivered")));
        AgentMessageRecord hourly = message(other, "fresh-chain", "queued");
        QVERIFY(fixture.journal->insertAgentMessage(hourly));
        // A request nobody answered in time.
        AgentMessageRecord overdue = message(receiver, "old-chain", "delivered");
        overdue.kind = "request";
        overdue.expectReply = true;
        overdue.delivery = "user_turn";
        overdue.expiresAt =
            QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODateWithMs);
        QVERIFY(fixture.journal->insertAgentMessage(overdue));

        fixture.restart();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.journal->agentMessage(chained.id)->state,
                                  QStringLiteral("held"), 5000);
        QCOMPARE(fixture.journal->agentMessage(chained.id)->error, "chain_limit");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.journal->agentMessage(hourly.id)->state,
                                  QStringLiteral("held"), 5000);
        QCOMPARE(fixture.journal->agentMessage(hourly.id)->error, "hourly_limit");
        QCOMPARE(fixture.journal->agentMessage(overdue.id)->state, "expired");
        bool expiredNotice = false;
        for (const QJsonObject& notice : fixture.payloads(sender, "agent_message.state"))
            expiredNotice = expiredNotice || (notice.value("message_id") == overdue.id &&
                                              notice.value("state") == "expired");
        QVERIFY(expiredNotice);
        QCOMPARE(fixture.count(receiver, "turn.completed"), 1);
        QCOMPARE(fixture.count(other, "turn.completed"), 1);
    }

    void sendValidatesTokensNumbersAndReplies() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "receiver");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        QCOMPARE(fixture.host->handleAgentCommand("forged", "agents.whoami", {})
                     .value("error_code")
                     .toString(),
                 "authentication_failed");
        const auto code = [&](const QJsonObject& payload) {
            return fixture.agents(sender, "agents.send", payload).value("error_code").toString();
        };
        QCOMPARE(code({{"to", fixture.number(sender)}, {"text", "me"}}), "self_message");
        QCOMPARE(code({{"to", "12"}, {"text", "x"}}), "invalid_number");
        const QString missing = fixture.number(sender) == "9999" ? "9998" : "9999";
        if (!fixture.journal->sessionForConversationNumber(missing))
            QCOMPARE(code({{"to", missing}, {"text", "x"}}), "unknown_number");
        QCOMPARE(code({{"to", fixture.number(receiver)}, {"text", "   "}}), "invalid_message");
        QCOMPARE(code({{"to", fixture.number(receiver)},
                       {"text", QString(AgentBroker::maximumBodyBytes + 1, 'x')}}),
                 "message_too_large");
        QCOMPARE(code({{"to", fixture.number(receiver)}, {"text", "x"}, {"reply_to", "nope"}}),
                 "invalid_reply");
        QCOMPARE(fixture.agents(sender, "agents.inbox", {{"message_id", "nope"}})
                     .value("error_code")
                     .toString(),
                 "message_not_found");
    }

    void deletionReleasesTheNumberRevokesTheTokenAndEndsMessages() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString sender = fixture.start("claude", "sender");
        const QString receiver = fixture.start("claude", "slow-start");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(sender, "turn.completed"), 1, 5000);
        const QString token = fixture.host->agentToken(receiver);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(receiver, "turn.completed"), 1, 5000);
        const QString number = fixture.number(receiver);
        const QJsonObject deleted =
            fixture.host->handleCommand(request("session.delete", {}, receiver, 1), "desktop");
        QVERIFY2(deleted.value("ok").toBool(), qPrintable(text(deleted)));
        QVERIFY(!fixture.journal->sessionForConversationNumber(number).has_value());
        QVERIFY(!fixture.host->isAgentToken(token));
        QCOMPARE(fixture.agents(sender, "agents.send", {{"to", number}, {"text", "gone?"}})
                     .value("error_code")
                     .toString(),
                 "unknown_number");
        // Quarantined: a new conversation does not receive the released number.
        const QString next = fixture.start("claude", "next");
        QVERIFY(fixture.number(next) != number);
    }

    void providersReceiveTheirOwnAgentServer() {
        Fixture fixture;
        QVERIFY(fixture.open(true));
        const QString claude = fixture.start("claude", "hello");
        const QString codex = fixture.start("codex", "hello");
        const QString cursor = fixture.start("cursor", "hello");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(claude, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(codex, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(cursor, "turn.completed"), 1, 8000);
        const QByteArray claudeLog = logText(fixture.claudeLog);
        QVERIFY(claudeLog.contains("\"ketplus_agents\""));
        QVERIFY(claudeLog.contains(fixture.host->agentToken(claude).toUtf8()));
        QVERIFY(claudeLog.contains("--mcp-agents"));
        QVERIFY(claudeLog.contains(("This conversation is #" + fixture.number(claude)).toUtf8()));
        const QByteArray codexLog = logText(fixture.codexLog);
        QVERIFY(codexLog.contains("\"mcp_servers.ketplus_agents.env\""));
        QVERIFY(codexLog.contains("\"mcp_servers.ketplus_agents.default_tools_approval_mode\""));
        QVERIFY(codexLog.contains(fixture.host->agentToken(codex).toUtf8()));
        QVERIFY(!codexLog.contains("\"mcp_servers\": {"));
        QVERIFY(codexLog.contains(("This conversation is #" + fixture.number(codex)).toUtf8()));
        const QByteArray cursorLog = logText(fixture.cursorLog);
        QVERIFY(cursorLog.contains("\"ketplus_agents\""));
        QVERIFY(cursorLog.contains(fixture.host->agentToken(cursor).toUtf8()));
        QVERIFY(cursorLog.contains(("This conversation is #" + fixture.number(cursor)).toUtf8()));
    }

    void agentConnectionsOnlyReachAgentOperations() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString codexPath = directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(codexPath, directory.filePath("codex.log")));
        DaemonConfiguration configuration;
        configuration.statePath = directory.filePath("journal.sqlite3");
        configuration.tokenPath = directory.filePath("local.token");
        configuration.socketName = directory.filePath("daemon.sock");
        configuration.codexExecutable = codexPath;
        DaemonService service(configuration);
        QVERIFY2(service.start(), qPrintable(service.errorString()));

        QLocalSocket desktop;
        desktop.connectToServer(directory.filePath("daemon.sock"));
        QVERIFY(desktop.waitForConnected(3000));
        QFile tokenFile(directory.filePath("local.token"));
        QVERIFY(tokenFile.open(QIODevice::ReadOnly));
        const auto roundTrip = [](QLocalSocket& socket, const QJsonObject& envelope) {
            socket.write(protocol::encodeFrame(envelope));
            QByteArray buffer;
            const QString id = envelope.value("message_id").toString();
            while (true) {
                while (buffer.size() < 4 ||
                       buffer.size() < 4 + ((static_cast<uchar>(buffer[0]) << 24) |
                                            (static_cast<uchar>(buffer[1]) << 16) |
                                            (static_cast<uchar>(buffer[2]) << 8) |
                                            static_cast<uchar>(buffer[3]))) {
                    if (!QTest::qWaitFor([&socket] { return socket.bytesAvailable() > 0; }, 5000))
                        return QJsonObject{};
                    buffer.append(socket.readAll());
                }
                const int size =
                    (static_cast<uchar>(buffer[0]) << 24) | (static_cast<uchar>(buffer[1]) << 16) |
                    (static_cast<uchar>(buffer[2]) << 8) | static_cast<uchar>(buffer[3]);
                const QJsonObject frame = QJsonDocument::fromJson(buffer.mid(4, size)).object();
                buffer.remove(0, 4 + size);
                if (frame.value("correlation_id").toString() == id)
                    return frame.value("payload").toObject();
            }
        };
        const auto hello = roundTrip(
            desktop, request("daemon.hello", {{"token", QString::fromUtf8(tokenFile.readAll())},
                                              {"client_id", "desktop"}}));
        QVERIFY(hello.value("ok").toBool());
        QVERIFY(hello.value("capabilities").toArray().contains("storage.retention.read"));
        QVERIFY(hello.value("capabilities").toArray().contains("storage.retention.apply"));
        QVERIFY(hello.value("capabilities").toArray().contains("storage.size.read"));
        QVERIFY(hello.value("capabilities").toArray().contains("storage.size.apply"));
        const auto size = roundTrip(desktop, request("storage.size.read"));
        QVERIFY(size.value("ok").toBool());
        const auto savedSize = roundTrip(
            desktop,
            request("storage.size.apply",
                    {{"expected_revision", size.value("revision")},
                     {"policy", QJsonObject{{"limit_bytes", 0}, {"warning_percent", 80}}}}));
        QVERIFY(savedSize.value("ok").toBool());
        QCOMPARE(savedSize.value("policy").toObject().value("limit_bytes").toInteger(), 0);
        const auto retention = roundTrip(desktop, request("storage.retention.read"));
        QVERIFY(retention.value("ok").toBool());
        const auto savedRetention = roundTrip(
            desktop, request("storage.retention.apply",
                             {{"expected_revision", retention.value("revision")},
                              {"policy", QJsonObject{{"full_detail_days", 90},
                                                     {"automatic_detail_retention", true}}}}));
        QVERIFY(savedRetention.value("ok").toBool());
        QCOMPARE(savedRetention.value("policy").toObject().value("full_detail_days").toInt(), 90);
        const QJsonObject started =
            roundTrip(desktop, request("session.start_turn",
                                       {{"worktree_path", directory.path()},
                                        {"prompt", "hello"},
                                        {"options", QJsonObject{{"provider", "codex"}}}}));
        QVERIFY2(started.value("ok").toBool(), qPrintable(text(started)));
        QVERIFY(!started.value("number").toString().isEmpty());
        // The injected server's environment names the token and the absolute endpoint.
        QByteArray providerLog;
        QTRY_VERIFY_WITH_TIMEOUT((providerLog = logText(directory.filePath("codex.log")))
                                     .contains("KETPLUS_AGENT_TOKEN"),
                                 5000);
        QVERIFY(providerLog.contains(directory.filePath("daemon.sock").toUtf8()));
        const QRegularExpression tokenPattern("\"KETPLUS_AGENT_TOKEN\": \"([0-9a-f]{64})\"");
        const QString token = tokenPattern.match(QString::fromUtf8(providerLog)).captured(1);
        QVERIFY(!token.isEmpty());

        QLocalSocket agent;
        agent.connectToServer(directory.filePath("daemon.sock"));
        QVERIFY(agent.waitForConnected(3000));
        QCOMPARE(roundTrip(agent, request("daemon.hello", {{"agent_token", "forged"}}))
                     .value("error_code")
                     .toString(),
                 "authentication_failed");
        agent.abort();
        agent.connectToServer(directory.filePath("daemon.sock"));
        QVERIFY(agent.waitForConnected(3000));
        QVERIFY(roundTrip(agent, request("daemon.hello", {{"agent_token", token}}))
                    .value("agent")
                    .toBool());
        QCOMPARE(roundTrip(agent, request("agents.whoami")).value("number").toString(),
                 started.value("number").toString());
        QCOMPARE(roundTrip(agent, request("session.list")).value("error_code").toString(),
                 "forbidden");
        QCOMPARE(roundTrip(agent, request("approval.resolve")).value("error_code").toString(),
                 "forbidden");
        QCOMPARE(roundTrip(agent, request("session.agent_messages.release"))
                     .value("error_code")
                     .toString(),
                 "forbidden");
        QCOMPARE(
            roundTrip(agent, request("session.agent_messages.read")).value("error_code").toString(),
            "forbidden");
        QCOMPARE(roundTrip(agent, request("storage.retention.read")).value("error_code").toString(),
                 "forbidden");
        QCOMPARE(
            roundTrip(agent, request("storage.retention.apply")).value("error_code").toString(),
            "forbidden");
        QCOMPARE(roundTrip(agent, request("storage.size.read")).value("error_code").toString(),
                 "forbidden");
        QCOMPARE(roundTrip(agent, request("storage.size.apply")).value("error_code").toString(),
                 "forbidden");
        // Session events are broadcast to the desktop, never to agent connections.
        agent.readAll();
        QTRY_VERIFY_WITH_TIMEOUT(desktop.bytesAvailable() > 0 || desktop.waitForReadyRead(50),
                                 5000);
        QTest::qWait(200);
        QCOMPARE(agent.bytesAvailable(), 0);
    }

    void mcpServerMapsToolsToAgentOperations() {
        QStringList operations;
        const agent_mcp::DaemonCall call = [&](const QString& operation,
                                               const QJsonObject& payload) {
            operations.append(operation);
            if (operation == "agents.send" && payload.value("to").toString() == "1234")
                return QJsonObject{{"ok", false},
                                   {"error_code", "unknown_number"},
                                   {"error_message", "No conversation"}};
            return QJsonObject{{"ok", true}, {"number", "4821"}};
        };
        const QJsonObject initialized =
            agent_mcp::handleMessage({{"jsonrpc", "2.0"},
                                      {"id", 1},
                                      {"method", "initialize"},
                                      {"params", QJsonObject{{"protocolVersion", "2025-03-26"}}}},
                                     call);
        QCOMPARE(initialized.value("result").toObject().value("protocolVersion").toString(),
                 "2025-03-26");
        QVERIFY(agent_mcp::handleMessage(
                    {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, call)
                    .isEmpty());
        const QJsonArray tools =
            agent_mcp::handleMessage({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}},
                                     call)
                .value("result")
                .toObject()
                .value("tools")
                .toArray();
        QCOMPARE(tools.size(), 9); // work_report, request_user_decision and schedule_*
        const QJsonObject whoami = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"},
             {"id", 3},
             {"method", "tools/call"},
             {"params", QJsonObject{{"name", "agents_whoami"}, {"arguments", QJsonObject{}}}}},
            call);
        QVERIFY(whoami.value("result")
                    .toObject()
                    .value("content")
                    .toArray()
                    .first()
                    .toObject()
                    .value("text")
                    .toString()
                    .contains("4821"));
        const QJsonObject failed = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"},
             {"id", 4},
             {"method", "tools/call"},
             {"params", QJsonObject{{"name", "agents_send"},
                                    {"arguments", QJsonObject{{"to", "1234"}, {"text", "x"}}}}}},
            call);
        QVERIFY(failed.value("result").toObject().value("isError").toBool());
        QCOMPARE(agent_mcp::handleMessage({{"jsonrpc", "2.0"},
                                           {"id", 5},
                                           {"method", "tools/call"},
                                           {"params", QJsonObject{{"name", "shell"}}}},
                                          call)
                     .value("error")
                     .toObject()
                     .value("code")
                     .toInt(),
                 -32602);
        // tools/list asks whoami whether to add the team tools (ADR 0114).
        QCOMPARE(operations, (QStringList{"agents.whoami", "agents.whoami", "agents.send"}));
        const auto callSchedule = [&](int id, const QString& name, const QJsonObject& arguments) {
            agent_mcp::handleMessage(
                {{"jsonrpc", "2.0"},
                 {"id", id},
                 {"method", "tools/call"},
                 {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}},
                call);
        };
        callSchedule(6, QStringLiteral("schedule_create"),
                     {{QStringLiteral("prompt"), QStringLiteral("Check")},
                      {QStringLiteral("inMinutes"), 1}});
        callSchedule(7, QStringLiteral("schedule_list"), {});
        callSchedule(8, QStringLiteral("schedule_cancel"),
                     {{QStringLiteral("id"), QStringLiteral("sched-1")}});
        QCOMPARE(operations, (QStringList{"agents.whoami", "agents.whoami", "agents.send",
                                          "agents.schedule_create", "agents.schedule_list",
                                          "agents.schedule_cancel"}));
    }
};

QTEST_GUILESS_MAIN(AgentMessagingTest)
#include "AgentMessagingTest.moc"
