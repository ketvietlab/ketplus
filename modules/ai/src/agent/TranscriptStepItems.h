#pragma once
#include <QJsonObject>
#include <QString>

namespace ketplus::agent_message {

// How provider tool items read as the steps of mockups/qt (TranscriptActivityItem.qml).

// Claude's Write, Edit and MultiEdit as the fileChange item Codex journals, so both read as
// "Created" or "Edited" with line counts. Empty for any other item.
[[nodiscard]] QJsonObject claudeFileChangeItem(const QJsonObject& item);
// What a step acted on, shown beside its title: the skill, the tools loaded, the subagent's
// task, the page or the query. Empty when the item names nothing.
[[nodiscard]] QString stepSubject(const QString& subtype, const QJsonObject& item);
// An MCP tool's server and operation, "ketplus_agents: agents_send", whether the provider
// names it `mcp__server__tool` or as separate fields.
[[nodiscard]] QString mcpToolSubject(const QJsonObject& item);
// Codex's own reading of a command: "Read: path", "Searched code: “query” in path" or
// "Listed files: path" with the step's subtype, from a command's single kind of action.
struct CommandStep {
    QString subtype;
    QString completedTitle;
    QString runningTitle;
};
[[nodiscard]] CommandStep commandActionStep(const QJsonObject& item);
// Codex web steps: "Searched the web: query", "Opened a page: url" or
// "Found in page: “pattern” on url".
[[nodiscard]] QString webStepTitle(const QJsonObject& item, bool completed);
// The KetPlus browser's steps as the mock titles them: "Opened: localhost:5173/settings",
// "Clicked: “Save”", "Waited for: text “Working”" or "Observed the page: Storage · 42 nodes".
// `operation` is the step's browser operation (open, act, wait…); empty for an unknown one.
[[nodiscard]] QString browserStepTitle(const QString& operation, const QJsonObject& item,
                                       bool completed);

} // namespace ketplus::agent_message
