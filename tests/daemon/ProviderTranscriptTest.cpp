#include "FakeCodex.h"
#include "agent/DaemonTranscriptNotices.h"
#include "agent/DaemonTranscriptProjection.h"
#include "cloud/CloudEventProjection.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorToolItem.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>
using namespace ketplus::daemon;
using namespace ketplus::agent_message;
class ProviderTranscriptTest : public QObject {
    Q_OBJECT
  private slots:
    void lifecycleNoticesAreProjectedWithoutInventingSuccess() {
        const QJsonArray events{
            QJsonObject{{"cursor", 1},
                        {"turn_sequence", 1},
                        {"event_type", "session.provider_changed"},
                        {"payload", QJsonObject{{"from", "codex"}, {"to", "claude"}}}},
            QJsonObject{{"cursor", 2},
                        {"turn_sequence", 1},
                        {"event_type", "session.configuration_changed"},
                        {"payload", QJsonObject{{"effective", QJsonObject{{"model", "test-model"},
                                                                          {"effort", "high"}}}}}},
            QJsonObject{{"cursor", 3},
                        {"turn_sequence", 1},
                        {"event_type", "conversation.context_changed"},
                        {"payload", QJsonObject{{"context_revision", 2}}}},
            QJsonObject{{"cursor", 4},
                        {"turn_sequence", 1},
                        {"event_type", "usage.session"},
                        {"payload", QJsonObject{{"estimatedCostUsd", 0.125}}}},
            QJsonObject{{"cursor", 5},
                        {"turn_sequence", 1},
                        {"event_type", "turn.completed"},
                        {"payload", QJsonObject{{"status", "completed"}}}}};
        const auto rows = daemonWindowMessages({{"events", events}});
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].toMap().value("title").toString(), "Provider changed");
        QCOMPARE(rows[1].toMap().value("body").toString(), "test-model · high");
        QCOMPARE(rows[2].toMap().value("body").toString(), "Context revision 2");
        QCOMPARE(rows[3].toMap().value("body").toString(), "$0.13 session");
        QCOMPARE(rows[3].toMap().value("messageKey").toString(), "daemon:1:notice:5");
        for (const auto& state : {"failed", "interrupted", "completed"}) {
            const auto note = daemonEventNotice("turn.completed", {{"status", state}}, "receipt");
            QCOMPARE(note.value("status").toString(), QString::fromLatin1(state));
            QVERIFY(note.value("body").toString().isEmpty());
        }
        QVERIFY(daemonEventNotice("turn.completed", {{"status", "unknown"}}, "receipt").isEmpty());
        QVERIFY(sessionEstimateText({{"estimatedCostUsd", -1}}).isEmpty());
        const auto retry = daemonEventNotice(
            "runtime.error", {{"will_retry", true}, {"message", "Network interrupted"}}, "retry");
        QCOMPARE(retry.value("subtype").toString(), "reconnecting");
        QCOMPARE(retry.value("status").toString(), "running");
        QVERIFY(daemonEventNotice("runtime.error", {{"message", "Fatal"}}, "error").isEmpty());
    }
    void turnEndReportsDurationContextAndSessionCost() {
        const auto at = [](int seconds) {
            return QDateTime(QDate(2026, 11, 9), QTime(1, 0), QTimeZone::UTC)
                .addSecs(seconds)
                .toString(Qt::ISODateWithMs);
        };
        const QJsonArray events{
            QJsonObject{{"cursor", 1},
                        {"turn_sequence", 1},
                        {"event_type", "user.message"},
                        {"payload", QJsonObject{{"text", "Fix the spinner"}}},
                        {"sent_at", at(0)}},
            QJsonObject{
                {"cursor", 2},
                {"turn_sequence", 1},
                {"event_type", "usage.context"},
                {"payload", QJsonObject{{"tokenUsage",
                                         QJsonObject{{"last", QJsonObject{{"totalTokens", 141200}}},
                                                     {"modelContextWindow", 200000}}}}},
                {"sent_at", at(200)}},
            QJsonObject{{"cursor", 3},
                        {"turn_sequence", 1},
                        {"event_type", "usage.session"},
                        {"payload", QJsonObject{{"estimatedCostUsd", 2.0812}}},
                        {"sent_at", at(370)}},
            QJsonObject{{"cursor", 4},
                        {"turn_sequence", 1},
                        {"event_type", "turn.completed"},
                        {"payload", QJsonObject{{"status", "completed"}}},
                        {"sent_at", at(380)}}};
        const auto rows = daemonWindowMessages({{"events", events}});
        QCOMPARE(rows.last().toMap().value("subtype").toString(), "turnEnd");
        QCOMPARE(rows.last().toMap().value("body").toString(),
                 QString::fromUtf8("6m 20s · 141k context · $2.08 session"));

        TurnFacts turn;
        QVERIFY(turnEndText(turn, {}).isEmpty());
        turn.durationMs = 44'600;
        QCOMPARE(turnEndText(turn, {}), "45s");
        turn.durationMs = 3'900'000;
        turn.contextTokens = 800;
        QCOMPARE(turnEndText(turn, {}), QString::fromUtf8("1h 05m · 800 context"));
        turn.durationMs = 68'000;
        turn.contextTokens = -1;
        QCOMPARE(turnEndText(turn, {{"estimatedCostUsd", -1}}), "1m 08s");
    }
    void agentMessagesShowTheirSenderAndHeldNotices() {
        const QJsonObject origin{
            {"origin", "agent"},
            {"text", "Please review"},
            {"agent_messages", QJsonArray{QJsonObject{{"message_id", "m1"},
                                                      {"kind", "request"},
                                                      {"from_number", "1093"},
                                                      {"from_title", "Parser"},
                                                      {"from_provider", "claude"},
                                                      {"body", "Please review"}}}}};
        const QJsonObject held{{"message_id", "m2"},
                               {"state", "held"},
                               {"reason", "hop_limit"},
                               {"direction", "incoming"},
                               {"peer_number", "1093"}};
        const auto rows = daemonWindowMessages(
            {{"events", QJsonArray{QJsonObject{{"event_type", "user.message"},
                                               {"cursor", 3},
                                               {"sent_at", "2026-11-01T12:00:00Z"},
                                               {"turn_sequence", 1},
                                               {"payload", origin}},
                                   QJsonObject{{"event_type", "agent_message.state"},
                                               {"cursor", 4},
                                               {"turn_sequence", 1},
                                               {"payload", held}}}}});
        QCOMPARE(rows.size(), 2);
        const auto message = rows.first().toMap();
        QCOMPARE(message.value("kind").toString(), "user");
        QCOMPARE(message.value("timestamp").toString(), "2026-11-01T12:00:00Z");
        QCOMPARE(message.value("subtype").toString(), "agentMessage");
        QCOMPARE(message.value("meta").toString(), "Message from @agent/1093 · Parser · Claude");
        QCOMPARE(message.value("body").toString(), "Please review");
        const auto notice = rows.last().toMap();
        QCOMPARE(notice.value("subtype").toString(), "heldAgentMessage");
        QCOMPARE(notice.value("sanitizedPayload").toMap().value("messageId").toString(), "m2");
        QCOMPARE(notice.value("title").toString(), agentMessageNoticeTitle());
        QVERIFY(notice.value("body").toString().contains("@agent/1093"));
        auto outgoingHeld = held;
        outgoingHeld.insert("direction", "outgoing");
        const auto outgoingRows = daemonWindowMessages(
            {{"events", QJsonArray{QJsonObject{{"event_type", "agent_message.state"},
                                               {"cursor", 5},
                                               {"payload", outgoingHeld}}}}});
        QCOMPARE(outgoingRows.size(), 1);
        QCOMPARE(outgoingRows.first().toMap().value("subtype").toString(), "notice");
        // A reply is labelled as one; an ordinary user message keeps "You".
        QVariantMap reply = message;
        markAgentOrigin(reply, QJsonArray{QJsonObject{{"kind", "reply"}, {"from_number", "2044"}}});
        QCOMPARE(reply.value("meta").toString(), "Reply from @agent/2044");
        QVariantMap own = message;
        own.insert("subtype", QString{});
        markAgentOrigin(own, {});
        QCOMPARE(own.value("subtype").toString(), QString{});
    }

    void agentMessageNoticesNameTheConversationAndDelivery() {
        QCOMPARE(agentMessageStateText({{"state", "sent"},
                                        {"kind", "request"},
                                        {"delivery", "starting"},
                                        {"expect_reply", true},
                                        {"peer_number", "1093"},
                                        {"peer_title", "Parser"}}),
                 "Sent to @agent/1093 · Parser · started a turn · reply requested.");
        QCOMPARE(agentMessageStateText(
                     {{"state", "sent"}, {"delivery", "queued"}, {"peer_number", "1093"}}),
                 "Sent to @agent/1093 · delivered when its turn ends.");
        QCOMPARE(agentMessageStateText({{"state", "sent"},
                                        {"delivery", "steered"},
                                        {"peer_number", "1093"},
                                        {"peer_title", "Parser"}}),
                 "Sent to @agent/1093 · Parser · added to its running turn.");
        QCOMPARE(agentMessageStateText({{"state", "sent"},
                                        {"kind", "reply"},
                                        {"delivery", "starting"},
                                        {"peer_number", "4821"},
                                        {"peer_title", "Release"}}),
                 "Replied to @agent/4821 · Release.");
        QCOMPARE(agentMessageStateText(
                     {{"state", "replied"}, {"peer_number", "4821"}, {"peer_title", "Release"}}),
                 "Sent as the reply to @agent/4821 · Release.");
    }

    // ADR 0067: review requests, skipped reviews and recorded reviews.
    void reviewNoticesShowTheRoundVerdictAndReviewer() {
        QCOMPARE(agentMessageStateText({{"state", "sent"},
                                        {"kind", "review"},
                                        {"delivery", "starting"},
                                        {"round", 1},
                                        {"max_rounds", 2},
                                        {"peer_number", "1093"},
                                        {"peer_title", "Reviewer for #4821"}}),
                 "Asked @agent/1093 · Reviewer for #4821 to review this turn (round 1 of 2).");
        QCOMPARE(agentMessageStateText(
                     {{"state", "review_skipped"}, {"kind", "review"}, {"reason", "no_changes"}}),
                 "No review: this turn changed no files.");
        QCOMPARE(agentMessageStateText({{"state", "review_skipped"}, {"reason", "round_limit"}}),
                 "No review: this exchange used all its review rounds.");
        QCOMPARE(agentMessageStateText(
                     {{"state", "review_skipped"}, {"reason", "reviewer_unsupported"}}),
                 "No review: only a Claude conversation can review turns.");
        QVERIFY(agentMessageStateText({{"state", "review_skipped"}, {"reason", "snapshot_failed"}})
                    .contains("next review includes them"));
        QCOMPARE(agentMessageStateText({{"state", "failed"},
                                        {"kind", "review"},
                                        {"reason", "reviewer_unsupported"},
                                        {"direction", "outgoing"},
                                        {"peer_number", "1093"}}),
                 "@agent/1093 no longer runs on Claude, so it did not review this turn.");
        const auto skipped = agentMessageNotice(
            {{"state", "review_skipped"}, {"kind", "review"}, {"reason", "reviewer_busy"}},
            "notice:7");
        QCOMPARE(skipped.value("subtype").toString(), "notice");
        QCOMPARE(skipped.value("title").toString(), "Review");

        // A recorded review is the reviewer's message, never a held-message action.
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{QJsonObject{{"event_type", "agent_message.state"},
                                     {"cursor", 8},
                                     {"turn_sequence", 2},
                                     {"payload", QJsonObject{{"message_id", "r1"},
                                                             {"kind", "reply"},
                                                             {"state", "review_recorded"},
                                                             {"verdict", "no_findings"},
                                                             {"body", "Looks fine.\nVerdict: no "
                                                                      "findings"},
                                                             {"direction", "incoming"},
                                                             {"peer_number", "1093"},
                                                             {"peer_title", "Reviewer"}}}}}}});
        QCOMPARE(rows.size(), 1);
        const auto review = rows.first().toMap();
        QCOMPARE(review.value("kind").toString(), "user");
        QCOMPARE(review.value("subtype").toString(), "agentMessage");
        QCOMPARE(review.value("meta").toString(),
                 "Review from @agent/1093 · Reviewer · No findings");
        QCOMPARE(review.value("body").toString(), "Looks fine.\nVerdict: no findings");
        QVERIFY(!review.value("sanitizedPayload").toMap().contains("messageId"));
        QCOMPARE(
            agentMessageNotice(
                {{"state", "review_recorded"}, {"verdict", "findings"}, {"peer_number", "1093"}},
                "notice:9")
                .value("meta")
                .toString(),
            "Review from @agent/1093 · Findings");

        // The reviewer's turn names what it was asked for.
        QVariantMap request{{"kind", "user"}};
        markAgentOrigin(request,
                        QJsonArray{QJsonObject{{"kind", "review"}, {"from_number", "4821"}}});
        QCOMPARE(request.value("meta").toString(), "Review request from @agent/4821");
    }

    void cursorPartialToolUpdatesKeepInputsAndResults() {
        auto raw = cursorToolUpdate({}, {{"toolCallId", "r"},
                                         {"kind", "read"},
                                         {"title", "Read a.txt"},
                                         {"rawInput", QJsonObject{{"path", "a.txt"}}},
                                         {"status", "in_progress"}});
        raw = cursorToolUpdate(
            raw,
            {{"toolCallId", "r"},
             {"status", "completed"},
             {"content", QJsonArray{QJsonObject{
                             {"type", "content"},
                             {"content", QJsonObject{{"type", "text"}, {"text", "File body"}}}}}}});
        const auto item = cursorToolItem(raw);
        QCOMPARE(item.value("type"), "dynamicToolCall");
        QCOMPARE(item.value("tool"), "read_file");
        QCOMPARE(item.value("arguments").toObject().value("path"), "a.txt");
        QCOMPARE(
            item.value("result").toObject().value("content").toArray().first().toObject().value(
                "text"),
            "File body");
    }
    void desktopHistoryKeepsCommentaryWithoutDuplicatingEarlierDeltas() {
        const auto event = [](const QString& type, const QJsonObject& payload, int cursor) {
            return QJsonObject{{"event_type", type},
                               {"payload", payload},
                               {"cursor", cursor},
                               {"turn_sequence", 1}};
        };
        const auto rows = daemonWindowMessages(
            {{"events", QJsonArray{event("assistant.delta",
                                         {{"item_id", "progress"}, {"delta", "Checking files"}}, 1),
                                   event("item.completed",
                                         {{"item_id", "progress"},
                                          {"item", QJsonObject{{"id", "progress"},
                                                               {"type", "agentMessage"},
                                                               {"phase", "commentary"},
                                                               {"text", "Checking files"}}}},
                                         2),
                                   event("item.completed",
                                         {{"item_id", "answer"},
                                          {"item", QJsonObject{{"id", "answer"},
                                                               {"type", "agentMessage"},
                                                               {"phase", "final_answer"},
                                                               {"text", "Done"}}}},
                                         3)}}});
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.first().toMap().value("body").toString(), QString("Checking files"));
        QCOMPARE(rows.last().toMap().value("body").toString(), QString("Done"));
    }
    void historyRestoresErrorsOutputAndAnsweredQuestions() {
        const auto event = [](const QString& type, const QJsonObject& data, int cursor) {
            return QJsonObject{
                {"event_type", type}, {"payload", data}, {"cursor", cursor}, {"turn_sequence", 1}};
        };
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{
                  event("runtime.error", {{"message", "Provider failed"}}, 1),
                  event("command.output_delta", {{"item_id", "c"}, {"delta", "First line\n"}}, 2),
                  event("command.output_delta", {{"item_id", "c"}, {"delta", "Second line"}}, 3),
                  event("approval.requested",
                        {{"approval_id", "q"},
                         {"kind", "input"},
                         {"questions",
                          QJsonArray{QJsonObject{{"id", "target"}, {"prompt", "Target?"}}}}},
                        4),
                  event("question.answered",
                        {{"approval_id", "q"}, {"answers", QJsonObject{{"target", "Desktop"}}}},
                        5)}}});
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).toMap().value("body"), "Provider failed");
        QCOMPARE(rows.at(1).toMap().value("details").toMap().value("output"),
                 "First line\nSecond line");
        QCOMPARE(rows.at(2).toMap().value("questionAnswers").toMap().value("target"), "Desktop");
    }
    void claudeSnapshotTwinsCollapseIntoTheStreamedAnswer() {
        int cursor = 0;
        const auto event = [&cursor](const QString& type, const QJsonObject& data) {
            return QJsonObject{{"event_type", type},
                               {"payload", data},
                               {"cursor", ++cursor},
                               {"turn_sequence", 1}};
        };
        const auto text = [](const QString& id, const QString& body, const QString& status) {
            return QJsonObject{{"item_id", id},
                               {"item", QJsonObject{{"id", id},
                                                    {"type", "agentMessage"},
                                                    {"text", body},
                                                    {"status", status}}}};
        };
        // Journaled by older sidecars: the snapshot ("-0") lands before the
        // stream's own completion ("-1").
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{
                  event("item.started", text("msg_a-1", "", "inProgress")),
                  event("assistant.delta", {{"item_id", "msg_a-1"}, {"delta", "Looking at it."}}),
                  event("item.completed", text("msg_a-0", "Looking at it.", "completed")),
                  event("item.completed", text("msg_a-1", "Looking at it.", "completed")),
                  event("item.completed", text("msg_b-0", "Different text.", "completed")),
                  event("item.completed", text("msg_b-1", "Another block.", "completed"))}}});
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0).toMap().value("body"), "Looking at it.");
        QCOMPARE(rows.at(0).toMap().value("messageKey"), daemonMessageKey(1, "msg_a-1"));
        QCOMPARE(rows.at(1).toMap().value("body"), "Different text.");
        QCOMPARE(rows.at(2).toMap().value("body"), "Another block.");
    }
    void receiptRestoresWithoutItsOriginalRequestPage() {
        const QJsonObject payload{
            {"approval_id", "q"},
            {"questions", QJsonArray{QJsonObject{{"id", "target"}, {"prompt", "Target?"}}}},
            {"answers", QJsonObject{{"target", "Desktop"}}}};
        const QJsonObject receipt{{"event_type", "question.answered"},
                                  {"cursor", 200},
                                  {"turn_sequence", 4},
                                  {"payload", payload}};
        const auto rows = daemonWindowMessages({{"events", QJsonArray{receipt}}});
        QCOMPARE(rows.size(), 1);
        const auto row = rows.first().toMap();
        QCOMPARE(row.value("questionAnswers").toMap().value("target"), "Desktop");
        QCOMPARE(row.value("questions").toList().first().toMap().value("question"), "Target?");
        QCOMPARE(row.value("messageKey").toString(), daemonMessageKey(1, "approval:q"));
    }

    void secretAnswersAreNeverStoredInReceipts() {
        ApprovalBroker broker;
        const auto id =
            broker.request("s", 1, 1, "input",
                           {{"questions", QJsonArray{QJsonObject{{"id", "secret"},
                                                                 {"question", "Value?"},
                                                                 {"isOther", true},
                                                                 {"isSecret", true},
                                                                 {"options", QJsonArray{}}}}}});
        QVERIFY(!id.isEmpty());
        const auto result = broker.resolve(
            id, "s", 1, "input",
            {{"request_id", id},
             {"revision", 1},
             {"action", "answer"},
             {"answers", QJsonArray{QJsonObject{{"question_id", "secret"},
                                                {"question_revision", 1},
                                                {"custom_answer", "sensitive-value"}}}}});
        QVERIFY(result.accepted);
        QCOMPARE(result.transcriptAnswers.value("secret"), "[redacted]");
        const auto cloud = projectEventPayloadForCloud(
            "question.answered", {{"approval_id", id},
                                  {"answers", QJsonObject{{"target", "Local answer"}}},
                                  {"questions", QJsonArray{QJsonObject{{"id", "target"}}}}});
        QVERIFY(!cloud.contains("answers"));
        QVERIFY(!cloud.contains("questions"));
    }
    void assistantAsyncQuestionsSteerTheirOwningTurn_data() {
        QTest::addColumn<bool>("finishBeforeDispatch");
        QTest::newRow("active-turn") << false;
        QTest::newRow("finished-before-queued-answer") << true;
    }
    void assistantAsyncQuestionsSteerTheirOwningTurn() {
        QFETCH(bool, finishBeforeDispatch);
        QTemporaryDir dir;
        const auto exe = dir.filePath("codex");
        QVERIFY(writeFakeCodex(exe, dir.filePath("log")));
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        CodexAdapter adapter(exe);
        SessionHost host(&journal, &adapter);
        const auto request = [](const QString& type, const QJsonObject& payload,
                                const QString& session = QString()) {
            return QJsonObject{{"message_type", type},
                               {"message_id", QUuid::createUuid().toString()},
                               {"payload", payload},
                               {"session_id", session},
                               {"session_generation", 1}};
        };
        const auto started = host.handleCommand(
            request("session.start_turn", {{"worktree_path", dir.path()}, {"prompt", "hang"}}),
            "test");
        QVERIFY(started.value("ok").toBool());
        const auto session = started.value("session_id").toString();
        QTRY_VERIFY(!journal.sessions().isEmpty() && journal.sessions().first().state == "running");
        const auto thread = journal.sessions().first().threadId;
        emit adapter.notificationReceived(
            "item/completed",
            {{"threadId", thread},
             {"item",
              QJsonObject{{"id", "async-question"},
                          {"type", "agentMessage"},
                          {"phase", "final_answer"},
                          {"text", "Choose target"},
                          {"delivery", "async"},
                          {"questions",
                           QJsonArray{QJsonObject{{"title", "Which target?"},
                                                  {"options", QJsonArray{"Desktop", "Web"}}}}}}}});
        const auto pending =
            host.handleCommand(request("session.pending_approvals", {}, session), "test")
                .value("pending_approvals")
                .toArray();
        QCOMPARE(pending.size(), 1);
        const auto card = pending.first().toObject();
        QVERIFY(!card.value("is_blocking").toBool());
        const auto question = card.value("questions").toArray().first().toObject();
        const auto resolved = host.handleCommand(
            request(
                "question.answer",
                {{"approval_id", card.value("approval_id")},
                 {"resolution",
                  QJsonObject{
                      {"request_id", card.value("approval_id")},
                      {"revision", 1},
                      {"action", "answer"},
                      {"answers",
                       QJsonArray{QJsonObject{
                           {"question_id", question.value("id")},
                           {"question_revision", 1},
                           {"option_ids",
                            QJsonArray{question.value("options").toArray().first().toObject().value(
                                "id")}}}}}}}},
                session),
            "test");
        QVERIFY(resolved.value("ok").toBool());
        if (finishBeforeDispatch) {
            emit adapter.notificationReceived(
                "turn/completed", {{"threadId", thread},
                                   {"turn", QJsonObject{{"id", journal.sessions().first().turnId},
                                                        {"status", "completed"}}}});
            QTest::qWait(50);
            QCOMPARE(journal.sessions().first().currentTurnSequence, 1);
            for (const auto& value : journal.eventsAfter(session, 1, 0, 100))
                QVERIFY(value.toObject().value("event_type") != "turn.steered");
            adapter.stop();
            return;
        }
        QTRY_VERIFY([&] {
            for (const auto& value : journal.eventsAfter(session, 1, 0, 100))
                if (value.toObject().value("event_type") == "turn.steered")
                    return true;
            return false;
        }());
        QCOMPARE(journal.sessions().first().currentTurnSequence, 1);
        adapter.stop();
    }
};
QTEST_GUILESS_MAIN(ProviderTranscriptTest)
#include "ProviderTranscriptTest.moc"
