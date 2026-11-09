#include "AgentTranscriptFixture.h"
#include "agent/DaemonTranscriptProjection.h"
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>

namespace ketplus::benchmark {
namespace {

// The mock's Claude answer (TranscriptFixtures.qml) with its shared-clock diagram.
const char* const claudeAnswer =
    "The sidebar now steps every working row on **one shared clock**: 20 steps a second in "
    "front, 4 behind another app.\n\n"
    "| Build | Idle CPU |\n|---|---|\n| Before | 41–47% |\n| After | 0–2% |\n\n"
    "```qml\nrotation: 360 * (KetTheme.animationClock % spinMs) / spinMs\n```\n\n"
    "Every indicator holds the same clock:\n\n"
    "```mermaid\nflowchart LR\n    subgraph Indicators\n        rows[\"Working rows\"]\n"
    "        bars[\"Activity bars\"]\n    end\n    clock((\"animationClock\"))\n"
    "    redraw[\"One window redraw\"]\n    rows -->|hold| clock\n    bars -->|hold| clock\n"
    "    clock -->|\"20 Hz in front\"| redraw\n    clock -.->|\"4 Hz behind\"| redraw\n```";

class Journal {
  public:
    explicit Journal(QString provider) : provider_(std::move(provider)) {}
    void user(const QDateTime& at, const QString& text, const QJsonObject& extra = {}) {
        QJsonObject payload{{"text", text}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            payload.insert(it.key(), it.value());
        append("user.message", payload, at, ++turn_);
        turnStart_ = at;
    }
    void agent(const QString& phase, const QString& text) {
        const QString id = phase + QString::number(cursor_ + 1);
        append(
            "item.completed",
            {{"provider", provider_},
             {"item_id", id},
             {"item",
              QJsonObject{{"type", "agentMessage"}, {"id", id}, {"phase", phase}, {"text", text}}}},
            {}, turn_);
    }
    // A completed tool step, shaped as the provider journals it.
    void tool(const QJsonObject& item) {
        const QString id = item.value("tool").toString(item.value("type").toString()) +
                           QString::number(cursor_ + 1);
        QJsonObject completed = item;
        completed.insert("id", id);
        if (!completed.contains("status"))
            completed.insert("status", "completed");
        append("item.completed", {{"provider", provider_}, {"item_id", id}, {"item", completed}},
               {}, turn_);
    }
    // A reasoning item that started a second after the prompt and took `seconds`.
    void thought(int seconds) {
        const QString id = QStringLiteral("reasoning%1").arg(cursor_ + 1);
        const QJsonObject item{{"type", "reasoning"}, {"id", id}};
        append("item.started", {{"provider", provider_}, {"item_id", id}, {"item", item}},
               turnStart_.addSecs(1), turn_);
        append("item.completed", {{"provider", provider_}, {"item_id", id}, {"item", item}},
               turnStart_.addSecs(1 + seconds), turn_);
    }
    // Any other journal event of the running turn, such as a runtime error.
    void event(const QString& type, const QJsonObject& payload) {
        append(type, payload, {}, turn_);
    }
    void steer(const QString& text) {
        append("turn.steered", {{"text", text}, {"provider", provider_}}, {}, turn_);
    }
    // The usage the provider reported during the turn: its context and the session estimate.
    void usage(qint64 contextTokens, double sessionUsd) {
        append("usage.context",
               {{"tokenUsage", QJsonObject{{"last", QJsonObject{{"totalTokens", contextTokens}}},
                                           {"modelContextWindow", 200000}}}},
               {}, turn_);
        if (sessionUsd >= 0)
            append("usage.session", {{"mode", "api_key"}, {"estimatedCostUsd", sessionUsd}}, {},
                   turn_);
    }
    // Ends the turn `seconds` after its prompt, as the mock's turn line reads.
    // `extra` carries the end's other fields, such as detail_trimmed or a Stop's reason.
    void finish(const QString& status, int seconds, QJsonObject extra = {}) {
        extra.insert("status", status);
        append("turn.completed", extra, turnStart_.addSecs(seconds), turn_);
    }
    QVariantList rows() const {
        QVariantList rows = agent_message::daemonWindowMessages({{"events", events_}});
        for (auto& row : rows) {
            auto entry = row.toMap();
            agent_message::labelProvider(entry, provider_);
            row = entry;
        }
        return rows;
    }

  private:
    void append(const QString& type, const QJsonObject& payload, const QDateTime& at, qint64 turn) {
        QJsonObject event{{"event_type", type},
                          {"cursor", ++cursor_},
                          {"turn_sequence", turn},
                          {"payload", payload}};
        if (at.isValid())
            event.insert("sent_at", at.toUTC().toString(Qt::ISODateWithMs));
        events_.append(event);
    }
    QString provider_;
    QJsonArray events_;
    qint64 cursor_ = 0;
    qint64 turn_ = 0;
    QDateTime turnStart_;
};

QJsonObject fromAgent(const QString& number, const QString& title, const QString& provider,
                      const QString& text) {
    return {{"origin", "agent"},
            {"agent_messages", QJsonArray{QJsonObject{{"message_id", "m" + number},
                                                      {"kind", "reply"},
                                                      {"from_number", number},
                                                      {"from_title", title},
                                                      {"from_provider", provider},
                                                      {"body", text}}}}};
}

} // namespace

QVariantList transcriptFixture(const QString& provider, const QString& assets) {
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime today = now.addSecs(-20 * 60);
    // The mock's workspace/error: a turn the provider stopped answering.
    if (provider == "error") {
        Journal journal("codex");
        journal.user(now.addSecs(-90), "Profile the editor startup on a cold cache.");
        journal.event("runtime.error",
                      {{"message", "Codex did not answer for 60 seconds. Your message and the "
                                   "work so far are kept."},
                       {"will_retry", false}});
        journal.finish("failed", 75);
        return journal.rows();
    }
    Journal journal(provider);
    if (provider == "claude") {
        journal.user(QDateTime(QDate(now.date().year(), 9, 24), QTime(9, 12)),
                     "Profile why the sidebar keeps the CPU busy when nothing happens.");
        journal.usage(141000, 2.08);
        // The mock's old turn: history retention has reduced its detail.
        journal.finish("completed", 380,
                       {{"detail_trimmed", QJsonObject{{"commands", 38},
                                                       {"tool_results", 12},
                                                       {"bytes", 1258291},
                                                       {"older_than_days", 30}}}});
        journal.user(today,
                     "The sidebar spinner still looks choppy. Check it against this capture and "
                     "fix it.");
        journal.agent("commentary",
                      "I'll read how the sidebar draws its working rows, then measure the idle "
                      "CPU.");
        // The mock's first group: Claude's Read, Grep and Bash steps.
        const auto text = [](const QString& value) {
            return QJsonObject{
                {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", value}}}}};
        };
        journal.tool(
            {{"type", "dynamicToolCall"},
             {"tool", "Read"},
             {"path", "modules/ai/src/agent/qml/AgentSidebar.qml"},
             {"arguments", QJsonObject{{"file_path", "modules/ai/src/agent/qml/AgentSidebar.qml"},
                                       {"offset", 540},
                                       {"limit", 61}}},
             {"result", text("Item {\n    id: row\n}")}});
        journal.tool(
            {{"type", "dynamicToolCall"},
             {"tool", "Grep"},
             {"arguments", QJsonObject{{"pattern", "Animation.Infinite"}, {"path", "modules/ai"}}},
             {"result", text("modules/ai/src/agent/qml/AgentRunStatus.qml:33\n"
                             "modules/ai/src/agent/qml/AgentSidebar.qml:579\n"
                             "modules/ai/src/agent/qml/BackgroundTaskBar.qml:149")}});
        journal.tool({{"type", "commandExecution"},
                      {"tool", "Bash"},
                      {"command", "top -l 4 -s 2 -pid 41087 -stats pid,cpu"},
                      {"description", "Measure idle CPU"},
                      {"aggregatedOutput", "41087  44.3\n41087  47.4\n41087  41.5\n41087  45.0"},
                      {"exitCode", 0}});
        journal.tool({{"type", "commandExecution"},
                      {"tool", "Bash"},
                      {"command", "ctest --test-dir build -R sidebar"},
                      {"description", "Run tests"},
                      {"status", "failed"},
                      {"exitCode", 8},
                      {"durationMs", 12400},
                      {"aggregatedOutput", "1/2 Test #31: sidebar_rows ........   Passed\n"
                                           "2/2 Test #32: sidebar_spin ........***Failed\n"
                                           "50% tests passed, 1 tests failed out of 2"}});
        journal.user(today.addSecs(120),
                     "20 steps a second reads as smooth at 14 px. Keep 4 behind another app.",
                     fromAgent("8941", "DS Icon", "claude",
                               "20 steps a second reads as smooth at 14 px. Keep 4 behind "
                               "another app."));
        journal.steer("Also check dark mode before you finish.");
        // The mock's edits: AgentSidebar.qml +3 −1 and a new SpinClock.qml of 48 lines.
        journal.tool({{"type", "dynamicToolCall"},
                      {"tool", "Edit"},
                      {"arguments",
                       QJsonObject{{"file_path", "modules/ai/src/agent/qml/AgentSidebar.qml"},
                                   {"old_string", "    RotationAnimation on rotation {}"},
                                   {"new_string", "    rotation: 360 * (KetTheme.animationClock % "
                                                  "spinMs) / spinMs\n    Component.onCompleted: "
                                                  "KetTheme.holdAnimationClock(true)\n    "
                                                  "Component.onDestruction: "
                                                  "KetTheme.holdAnimationClock(false)"}}},
                      {"result", text("The file was updated.")}});
        QStringList spinClock;
        for (int line = 1; line <= 48; ++line)
            spinClock.append(QStringLiteral("// SpinClock line %1").arg(line));
        journal.tool(
            {{"type", "dynamicToolCall"},
             {"tool", "Write"},
             {"arguments", QJsonObject{{"file_path", "modules/ai/src/agent/qml/SpinClock.qml"},
                                       {"content", spinClock.join('\n')}}},
             {"result", text("The file was created.")}});
        journal.agent("final_answer", QString::fromUtf8(claudeAnswer));
        journal.usage(182000, 3.32);
        journal.finish("completed", 252);
    } else if (provider == "cursor") {
        journal.event("session.forked", {{"from_conversation_number", "5208"},
                                         {"through_turn", 14},
                                         {"provider", "cursor"},
                                         {"model", "Composer 2"}});
        journal.user(today, "Find where the staging tenant gets created and summarise the flow.");
        journal.tool({{"type", "commandExecution"},
                      {"command", "rg -n \"createTenant\" services"},
                      {"aggregatedOutput", "services/tenant/src/create.ts:18\n"
                                           "services/gateway/src/routes/tenant.ts:42"},
                      {"exitCode", 0}});
        journal.tool({{"type", "commandExecution"},
                      {"command", "sed -n 1,80p services/tenant/src/create.ts"},
                      {"exitCode", 0}});
        journal.tool({{"type", "collabAgentToolCall"},
                      {"tool", "Task"},
                      {"prompt", "Trace the staging tenant flow"},
                      {"description", "Trace the staging tenant flow"}});
        journal.tool({{"type", "mcpToolCall"},
                      {"server", "ketplus_browser"},
                      {"tool", "browser_observe"},
                      {"arguments", QJsonObject{{"url", "https://staging-admin.example"}}}});
        journal.agent("final_answer",
                      "A staging tenant is created in **two steps**:\n\n1. `POST /tenants` in the "
                      "gateway validates the slug.\n2. `createTenant()` provisions the schema and "
                      "seeds the admin user.");
        journal.event(
            "runtime.error",
            {{"message", "The agent process ended with code 1 before the turn completed."},
             {"will_retry", false}});
        journal.finish("failed", 151);
    } else {
        journal.event(
            "session.provider_changed",
            {{"from", "claude"}, {"to", "codex"}, {"model", "GPT-5.6 Luna"}, {"effort", "High"}});
        journal.user(today, "Add a journal size warning to the daemon status and the sidebar.");
        // The mock's first group: a thought, two reads, a search and a listing.
        journal.thought(14);
        const auto action = [](const QJsonObject& value) {
            return QJsonObject{{"type", "commandExecution"},
                               {"command", value.value("command")},
                               {"commandActions", QJsonArray{value}},
                               {"exitCode", 0}};
        };
        journal.tool(
            action({{"type", "read"},
                    {"path", "apps/daemon/src/storage/LocalJournal.cpp"},
                    {"command", "sed -n 760,860p apps/daemon/src/storage/LocalJournal.cpp"}}));
        journal.tool(action({{"type", "read"},
                             {"path", "apps/daemon/src/storage/LocalJournal.h"},
                             {"command", "sed -n 1,120p apps/daemon/src/storage/LocalJournal.h"}}));
        journal.tool(action({{"type", "search"},
                             {"query", "pruneRetention"},
                             {"path", "apps/daemon"},
                             {"command", "rg -n pruneRetention apps/daemon"}}));
        journal.tool(action({{"type", "listFiles"},
                             {"path", "modules/ai/src/agent/qml"},
                             {"command", "ls modules/ai/src/agent/qml"}}));
        journal.agent("commentary",
                      "Retention never reclaims free pages. I'll measure the journal before "
                      "changing it.");
        journal.tool(
            {{"type", "commandExecution"},
             {"command",
              "sqlite3 -readonly journal.sqlite3 \"pragma page_count; pragma freelist_count\""},
             {"cwd", "~/Library/Application Support/Ketsuite/KetPlus/daemon"},
             {"aggregatedOutput", "2205637\n1261337"},
             {"durationMs", 200},
             {"exitCode", 0}});
        journal.tool({{"type", "commandExecution"},
                      {"command", "cmake --build build --target ketplus_daemon"},
                      {"aggregatedOutput",
                       "[412/980] Building CXX object apps/daemon/CMakeFiles/…/LocalJournal.cpp.o\n"
                       "[413/980] Building CXX object apps/daemon/CMakeFiles/…/SessionHost.cpp.o"},
                      {"durationMs", 38000},
                      {"exitCode", 0}});
        // The mock's edits: an update, a new file, a move and a deletion.
        QStringList budget;
        for (int line = 1; line <= 41; ++line)
            budget.append(QStringLiteral("+// JournalBudget line %1").arg(line));
        QStringList removed;
        for (int line = 1; line <= 120; ++line)
            removed.append(QStringLiteral("-// OldRetention line %1").arg(line));
        QStringList added;
        for (int line = 1; line <= 64; ++line)
            added.append(QStringLiteral("+    // free pages %1").arg(line));
        journal.tool(
            {{"type", "fileChange"},
             {"changes",
              QJsonArray{QJsonObject{{"path", "apps/daemon/src/storage/LocalJournal.cpp"},
                                     {"kind", QJsonObject{{"type", "update"}}},
                                     {"diff", "@@ -794,6 +794,66 @@\n-    old();\n-    older();\n" +
                                                  added.join('\n')}},
                         QJsonObject{{"path", "apps/daemon/src/storage/JournalBudget.h"},
                                     {"kind", QJsonObject{{"type", "add"}}},
                                     {"diff", budget.join('\n')}},
                         QJsonObject{{"path", "docs/journal.md"},
                                     {"kind", QJsonObject{{"type", "update"},
                                                          {"move_path", "docs/daemon-journal.md"}}},
                                     {"diff", ""}},
                         QJsonObject{{"path", "apps/daemon/src/storage/OldRetention.cpp"},
                                     {"kind", QJsonObject{{"type", "delete"}}},
                                     {"diff", removed.join('\n')}}}}});
        // The mock's browser and MCP steps.
        journal.tool(
            {{"type", "mcpToolCall"},
             {"server", "ketplus_browser"},
             {"tool", "browser_open"},
             {"arguments", QJsonObject{{"url", "http://localhost:5173/settings/storage"}}}});
        journal.tool(
            {{"type", "mcpToolCall"},
             {"server", "ketplus_browser"},
             {"tool", "browser_observe"},
             {"arguments", QJsonObject{{"detail", "summary"}}},
             {"result", QJsonObject{{"structuredContent",
                                     QJsonObject{{"page", QJsonObject{{"title", "Storage"}}},
                                                 {"visibleNodeCount", 4}}}}}});
        journal.tool({{"type", "mcpToolCall"},
                      {"server", "node_repl"},
                      {"tool", "js"},
                      {"arguments", QJsonObject{{"code", "format the size table"}}},
                      {"durationMs", 400}});
        journal.tool({{"type", "mcpToolCall"},
                      {"server", "ketplus_agents"},
                      {"tool", "agents_list"},
                      {"status", "failed"},
                      {"error", QJsonObject{{"message", "the daemon closed the connection"}}}});
        // The mock's web, image and wait steps.
        journal.tool({{"type", "webSearch"},
                      {"query", "sqlite incremental_vacuum free pages"},
                      {"action", QJsonObject{{"type", "search"},
                                             {"query", "sqlite incremental_vacuum free pages"}}}});
        journal.tool({{"type", "webSearch"},
                      {"action", QJsonObject{{"type", "openPage"},
                                             {"url", "https://sqlite.org/lang_vacuum.html"}}}});
        journal.tool({{"type", "webSearch"},
                      {"action", QJsonObject{{"type", "findInPage"},
                                             {"url", "https://sqlite.org/lang_vacuum.html"},
                                             {"pattern", "VACUUM INTO"}}}});
        journal.tool({{"type", "imageView"}, {"path", "docs/assets/storage-mock.png"}});
        journal.tool({{"type", "imageGeneration"},
                      {"revisedPrompt",
                       "A flat storage gauge icon in the Két style, 2 px strokes, no frame."}});
        journal.event("runtime.error", {{"message", "Reconnecting… 2/5"}, {"will_retry", true}});
        journal.event("runtime.error",
                      {{"message", "Codex could not finish the turn. Try again, or pick another "
                                   "model for this conversation."},
                       {"will_retry", false}});
        journal.event("session.configuration_changed",
                      {{"effective", QJsonObject{{"model", "GPT-5.6 Luna"}, {"effort", "Medium"}}},
                       {"changed", QJsonArray{"model", "effort"}}});
        journal.event("conversation.context_changed", {{"context_revision", 2},
                                                       {"branch", "feat/journal-storage"},
                                                       {"worktree_name", "ketplus-journal"}});
        journal.agent("final_answer",
                      "The daemon now reports `journal.size` in `session.list` and warns past "
                      "**80%** of the limit. Still to do:");
        journal.usage(96000, -1);
        // The mock's Stop from the composer.
        journal.finish("interrupted", 68, {{"reason", "user"}});
    }
    QVariantList rows = journal.rows();
    // The mock's first Claude prompt today carries a capture; journal media needs a store, so
    // the row takes the screenshot's file URL directly.
    if (provider == "claude") {
        const QString shot =
            QUrl::fromLocalFile(QDir(assets).filePath("screenshot-sidebar.png")).toString();
        for (auto& row : rows) {
            auto entry = row.toMap();
            if (entry.value("kind") == "user" &&
                entry.value("body").toString().contains("capture")) {
                entry.insert("images", QVariantList{shot});
                row = entry;
                break;
            }
        }
    }
    return rows;
}

} // namespace ketplus::benchmark
