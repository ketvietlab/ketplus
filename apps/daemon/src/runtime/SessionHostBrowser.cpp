#include "runtime/SessionHost.h"

#include "CodexBrowserTools.h"
#include "providers/CodexAdapter.h"

namespace ketplus::daemon {

// Agents are told to close every browser session before their final answer, but a model can
// forget. Codex keeps a thread's MCP servers alive across turns, so a forgotten session would
// hold its browser context until the thread unloads. The daemon closes them when the turn ends.

void SessionHost::noteBrowserToolItem(Session& session, const QJsonObject& item) {
    if (item.value(QStringLiteral("type")).toString() == QStringLiteral("mcpToolCall") &&
        item.value(QStringLiteral("server")).toString() == QLatin1String(browser_tools::serverName))
        session.browserUsedThisTurn = true;
}

void SessionHost::closeTurnBrowserSessions(Session& session) {
    // Only the server KetPlus injected answers to this tool; a turn that never called it has
    // nothing open, and calling would start an idle server just to close nothing.
    if (!std::exchange(session.browserUsedThisTurn, false) ||
        adapter_->launchedBrowserArguments().isEmpty() || session.stored.threadId.isEmpty())
        return;
    const QString sessionId = session.stored.id;
    adapter_->call(
        QStringLiteral("mcpServer/tool/call"),
        {{QStringLiteral("threadId"), session.stored.threadId},
         {QStringLiteral("server"), QLatin1String(browser_tools::serverName)},
         {QStringLiteral("tool"), QStringLiteral("browser_close_all_sessions")},
         {QStringLiteral("arguments"), QJsonObject{}}},
        [sessionId](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty() || result.value(QStringLiteral("isError")).toBool())
                qWarning("Closing browser sessions after a turn of %s failed: %s",
                         qUtf8Printable(sessionId),
                         qUtf8Printable(error.isEmpty() ? QStringLiteral("tool error") : error));
        });
}

} // namespace ketplus::daemon
