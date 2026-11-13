#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"

#include <QJsonArray>
#include <QQuickView>
#include <QtTest>

using namespace agent_workspace_test;

namespace {
// The text of every visible item under `item`: what the reader can see.
void visibleTexts(QQuickItem* item, QStringList& texts) {
    if (!item || !item->isVisible() || item->opacity() <= 0)
        return;
    if (const QVariant text = item->property("text"); text.isValid())
        texts.append(text.toString());
    for (auto* child : item->childItems())
        visibleTexts(child, texts);
}

QJsonObject journaled(qint64 cursor, qint64 turn, const QString& type, const QJsonObject& payload) {
    return {
        {"cursor", cursor}, {"turn_sequence", turn}, {"event_type", type}, {"payload", payload}};
}

QJsonObject toolCall(const QString& id, const QString& tool, const QString& server = {}) {
    QJsonObject item{{"id", id},
                     {"type", server.isEmpty() ? "dynamicToolCall" : "mcpToolCall"},
                     {"tool", tool},
                     {"status", "completed"},
                     {"arguments", QJsonObject{{"to", "8941"}, {"text", "Please review T-78"}}}};
    if (!server.isEmpty())
        item.insert("server", server);
    return {{"item", item}};
}
} // namespace

// T-78: a transcript of team plumbing around real work draws only the work: no "Tasks Changed"
// row, no raw mcp__ketplus_agents__* step, no blank activity group, and the browser step, the
// incoming agent card and the agent's words as before.
void AgentWorkspaceViewTest::internalStepsAreNotDrawn() {
    const QJsonArray events{
        journaled(1, 1, "user.message", {{"text", "Ask the reviewer to look at T-78"}}),
        journaled(2, 1, "tasks.changed", {{"tasks", QJsonArray{}}}),
        journaled(3, 1, "item.completed", toolCall("send", "mcp__ketplus_agents__agents_send")),
        journaled(4, 1, "tasks.changed", {{"tasks", QJsonArray{}}}),
        journaled(5, 1, "item.completed", toolCall("board", "team_tasks", "ketplus_agents")),
        journaled(
            6, 1, "item.completed",
            {{"item", QJsonObject{{"id", "open"},
                                  {"type", "dynamicToolCall"},
                                  {"tool", "mcp__ketplus_browser__browser_open"},
                                  {"status", "completed"},
                                  {"arguments", QJsonObject{{"url", "https://example.test/"}}}}}}),
        journaled(7, 1, "item.completed",
                  {{"item", QJsonObject{{"id", "say"},
                                        {"type", "agentMessage"},
                                        {"phase", "final_answer"},
                                        {"text", "I asked the reviewer."}}}}),
        journaled(8, 1, "turn.completed", {{"status", "completed"}}),
        journaled(9, 2, "user.message",
                  {{"text", "Looks good to me."},
                   {"origin", "agent"},
                   {"agent_messages", QJsonArray{QJsonObject{{"message_id", "r1"},
                                                             {"kind", "reply"},
                                                             {"from_number", "8941"},
                                                             {"from_title", "Reviewer"},
                                                             {"from_provider", "claude"}}}}}),
        // A turn of only internal calls: nothing, not an empty group.
        journaled(10, 2, "item.completed", toolCall("report", "mcp__ketplus_agents__work_report")),
        journaled(11, 2, "item.completed", toolCall("inbox", "agents_inbox", "ketplus_agents")),
        journaled(12, 2, "item.completed",
                  {{"item", QJsonObject{{"id", "done"},
                                        {"type", "agentMessage"},
                                        {"phase", "final_answer"},
                                        {"text", "The reviewer agreed."}}}}),
        journaled(13, 2, "turn.completed", {{"status", "completed"}})};
    const auto rows = ketplus::agent_message::daemonWindowMessages({{"events", events}});
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(760, 900);
    view.show();
    view.resetMessages(rows, "internal-steps");
    auto* root = view.quickView()->rootObject();
    QTRY_VERIFY(findVisibleItem(root, "transcriptAgentNote") != nullptr);
    QStringList texts;
    QTRY_VERIFY((texts.clear(), visibleTexts(root, texts),
                 texts.join('\n').contains("The reviewer agreed.")));
    // Before and after captures of the same transcript (KETPLUS_T78_CAPTURE=<file>.png).
    if (const QString capture = qEnvironmentVariable("KETPLUS_T78_CAPTURE"); !capture.isEmpty()) {
        QTest::qWait(400);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
    const QString shown = texts.join('\n');
    for (const auto* internal : {"ketplus_agents", "agents_send", "team_tasks", "work_report",
                                 "agents_inbox", "Tasks Changed", "tasks changed"})
        QVERIFY2(!shown.contains(QLatin1String(internal), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("drawn: ") + QLatin1String(internal)));
    QVERIFY(shown.contains("Ask the reviewer to look at T-78"));
    QVERIFY(shown.contains("I asked the reviewer."));
    QVERIFY(shown.contains("example.test"));
    // The rows are the work: the browser step is the only step left.
    int steps = 0;
    for (const auto& row : rows)
        steps += row.toMap().value("kind") == "tool" && row.toMap().value("subtype") != "turnEnd";
    QCOMPARE(steps, 1);
}
