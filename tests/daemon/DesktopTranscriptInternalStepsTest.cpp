#include "TranscriptWindowTestSupport.h"

#include <QStandardPaths>

// T-78: KetPlus's own agents tools (mcp__ketplus_agents__*) and the background task snapshot
// (tasks.changed) never show as transcript rows, from a history window or live. Another
// server's tool, an incoming agent message card and the agent's words still do.
class DesktopTranscriptInternalStepsTest final : public QObject {
    Q_OBJECT

    // Every string a row carries, so a hidden step cannot hide in a title or a detail.
    static QString rowText(const QVariant& row) {
        QString text;
        const auto add = [&](const auto& self, const QVariant& value) -> void {
            if (value.typeId() == QMetaType::QVariantMap)
                for (const auto& field : value.toMap())
                    self(self, field);
            else if (value.typeId() == QMetaType::QVariantList)
                for (const auto& field : value.toList())
                    self(self, field);
            else if (value.canConvert<QString>())
                text += value.toString() + QLatin1Char('\n');
        };
        add(add, row);
        return text;
    }

    static QJsonObject tool(const QString& id, const QString& type, const QString& name,
                            const QString& server = {}) {
        QJsonObject item{
            {"id", id},
            {"type", type},
            {"tool", name},
            {"status", "completed"},
            {"arguments", QJsonObject{{"to", "8941"}, {"text", "Hello"}}},
            {"result",
             QJsonObject{{"content",
                          QJsonArray{QJsonObject{{"type", "text"}, {"text", "agents_send ok"}}}}}}};
        if (!server.isEmpty())
            item.insert("server", server);
        return item;
    }

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void internalStepsStayOutOfHistoryAndTheLiveView() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "internal-steps";
        rememberConversation(path, id);
        const QJsonObject send =
            tool("send", "dynamicToolCall", "mcp__ketplus_agents__agents_send");
        auto sendStarted = send;
        sendStarted.insert("status", "inProgress");
        sendStarted.remove("result");
        const QJsonObject browser{{"id", "browser"},
                                  {"type", "dynamicToolCall"},
                                  {"tool", "mcp__ketplus_browser__browser_open"},
                                  {"status", "completed"},
                                  {"arguments", QJsonObject{{"url", "https://example.test/"}}}};
        const QJsonObject report{
            {"title", "Hide the steps"},
            {"turn", 2},
            {"version", 1},
            {"items", QJsonArray{QJsonObject{
                          {"key", "t-78"},
                          {"title", "Hide the steps"},
                          {"status", "active"},
                          {"next", QJsonObject{{"action", "Review"}, {"owner", "user"}}}}}}};
        const QJsonArray history{
            windowEvent(id, "user.message", {{"text", "Ask the reviewer"}}, 1, 1),
            windowEvent(id, "tasks.changed", {{"tasks", QJsonArray{}}}, 2, 1),
            windowEvent(id, "item.started", {{"item", sendStarted}}, 3, 1),
            windowEvent(id, "item.completed", {{"item", send}}, 4, 1),
            windowEvent(id, "item.completed",
                        {{"item", tool("tasks", "mcpToolCall", "team_tasks", "ketplus_agents")}}, 5,
                        1),
            windowEvent(id, "item.completed", {{"item", browser}}, 6, 1),
            windowEvent(id, "tasks.changed", {{"tasks", QJsonArray{}}}, 7, 1),
            windowEvent(id, "user.message",
                        {{"text", "Looks good."},
                         {"origin", "agent"},
                         {"agent_messages", QJsonArray{QJsonObject{{"message_id", "r1"},
                                                                   {"kind", "reply"},
                                                                   {"from_number", "8941"},
                                                                   {"from_title", "Reviewer"},
                                                                   {"body", "Looks good."}}}}},
                        8, 2),
            // A decision card and the work report stay: they are not team plumbing.
            windowEvent(id, "approval.requested",
                        {{"approval_id", "a1"},
                         {"kind", "approval"},
                         {"request_type", "command"},
                         {"reason", "Codex wants network access."},
                         {"command", "curl https://example.test"},
                         {"revision", 1}},
                        9, 2),
            windowEvent(id, "approval.resolved",
                        {{"approval_id", "a1"}, {"answers", QJsonObject{{"decision", "accept"}}}},
                        10, 2),
            windowEvent(id, "work_report.updated", report, 11, 2),
            windowEvent(id, "item.completed",
                        {{"item", QJsonObject{{"id", "answer"},
                                              {"type", "agentMessage"},
                                              {"phase", "final_answer"},
                                              {"text", "The reviewer agreed."}}}},
                        12, 2),
            windowEvent(id, "turn.completed", {{"status", "completed"}}, 13, 2)};
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"transcript_window_max_turns", 20},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 13}}}}};
                if (type != "session.transcript_window")
                    return {};
                Q_UNUSED(envelope);
                return {{"transcript_window_version", 1},
                        {"session_id", id},
                        {"session_generation", 1},
                        {"snapshot_cursor", 13},
                        {"projection_revision", 1},
                        {"current_turn_sequence", 2},
                        {"state", "ready"},
                        {"pending_approvals", QJsonArray{}},
                        {"events", history},
                        {"before_cursor", 1},
                        {"has_more_before", false}};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.messages(path).isEmpty(), 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());

        const auto check = [&](const QStringList& bodies) {
            QStringList seen;
            bool browserRow = false;
            bool agentCard = false;
            bool approvalCard = false;
            bool reportCard = false;
            for (const auto& row : desktop.messages(path)) {
                const QString text = rowText(row);
                if (text.contains("ketplus_agents") || text.contains("agents_send") ||
                    text.contains("team_tasks") ||
                    text.contains("tasks changed", Qt::CaseInsensitive))
                    return QStringLiteral("internal step shown: ") + text.simplified();
                const auto map = row.toMap();
                browserRow = browserRow || map.value("eventType") == "browser.open";
                agentCard = agentCard || map.value("subtype") == "agentMessage";
                approvalCard = approvalCard || (map.value("subtype") == "approval" &&
                                                map.value("status") == "approved");
                reportCard = reportCard ||
                             !map.value("workReport").toMap().value("items").toList().isEmpty();
                if (map.value("kind") == "assistant" || map.value("kind") == "user")
                    seen.append(map.value("body").toString());
            }
            if (!browserRow)
                return QStringLiteral("the browser step is missing");
            if (!agentCard)
                return QStringLiteral("the agent message card is missing");
            if (!approvalCard)
                return QStringLiteral("the decided approval card is missing");
            if (!reportCard)
                return QStringLiteral("the work report card is missing");
            for (const auto& body : bodies)
                if (!seen.contains(body))
                    return QStringLiteral("missing text: ") + body;
            return QString();
        };
        QString problem = check({"Ask the reviewer", "The reviewer agreed."});
        QVERIFY2(problem.isEmpty(), qPrintable(problem));

        // Live: a turn of internal calls around the agent's words adds only the words.
        const int before = desktop.messages(path).size();
        daemon.push(liveEvent(id, "user.message", {{"text", "Tell the team"}}, 14, 3));
        daemon.push(liveEvent(id, "tasks.changed", {{"tasks", QJsonArray{}}}, 15, 3));
        daemon.push(liveEvent(id, "item.started", {{"item", sendStarted}}, 16, 3));
        daemon.push(liveEvent(id, "item.completed", {{"item", send}}, 17, 3));
        daemon.push(liveEvent(
            id, "item.completed",
            {{"item", tool("report", "dynamicToolCall", "mcp__ketplus_agents__work_report")}}, 18,
            3));
        daemon.push(liveEvent(id, "item.completed",
                              {{"item", QJsonObject{{"id", "live-answer"},
                                                    {"type", "agentMessage"},
                                                    {"phase", "final_answer"},
                                                    {"text", "Sent to the team."}}}},
                              19, 3));
        QTRY_VERIFY(rowText(desktop.messages(path).last()).contains("Sent to the team."));
        problem = check({"Tell the team", "Sent to the team."});
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(desktop.messages(path).size(), before + 2);
    }
};

QTEST_GUILESS_MAIN(DesktopTranscriptInternalStepsTest)
#include "DesktopTranscriptInternalStepsTest.moc"
