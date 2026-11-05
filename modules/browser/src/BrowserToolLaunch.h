#pragma once

#include "ToolPackageRegistry.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

namespace ketplus::browser_tools {

// Shared host policy for every provider. Missing runtime, disabled tools or invalid
// settings return no launch; provider-supplied turn input never selects a server.
[[nodiscard]] std::optional<ToolLaunch> configuredBrowserLaunch(const Settings& settings,
                                                                const QString& searchStart,
                                                                const QString& artifactsDirectory);
[[nodiscard]] std::optional<ToolLaunch> browserLaunchFromUserSettings(const QString& searchStart);
[[nodiscard]] QJsonObject claudeMcpServers(const std::optional<ToolLaunch>& launch);
[[nodiscard]] QJsonArray acpMcpServers(const std::optional<ToolLaunch>& launch);

// A host that cannot call the server's tools itself (ACP) ends a conversation's turn by writing
// the time to this file; the server then closes the browser sessions opened before it
// (apps/browser/src/mcp.mjs). One file per conversation, under the launch's artifacts directory.
inline constexpr auto turnEndFileVariable = "KETPLUS_BROWSER_TURN_END_FILE";
[[nodiscard]] QString turnEndSignalFile(const QString& artifactsDirectory,
                                        const QString& conversationId);
bool signalTurnEnd(const QString& path);

[[nodiscard]] QString
browserSkillFile(const Settings& settings, const QString& activePath,
                 const QString& applicationDir = QCoreApplication::applicationDirPath());

} // namespace ketplus::browser_tools
