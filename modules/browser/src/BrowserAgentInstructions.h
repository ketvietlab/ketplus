#pragma once

#include <QJsonObject>
#include <QString>

namespace ketplus::browser_tools {

// Availability must come from the provider's captured MCP launch, never turn input.
[[nodiscard]] QString browserAgentInstructions(bool available);
// Guidance to read files and tool output narrowly; it applies with or without a browser.
[[nodiscard]] QString agentReadingInstructions();
// Shared diagram authoring guidance, independent of browser availability.
[[nodiscard]] QString agentMermaidInstructions();
// Everything KetPlus adds to an agent's instructions: the browser, reading and Mermaid policies
// and, when the application ships them, when to read the default skills.
[[nodiscard]] QString hostAgentInstructions(bool browserAvailable);
// Appends hostAgentInstructions to Codex thread/start and thread/resume.
[[nodiscard]] QJsonObject withCodexBrowserInstructions(const QString& method, QJsonObject params,
                                                       bool available);

} // namespace ketplus::browser_tools
