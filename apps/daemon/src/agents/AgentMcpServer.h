#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace ketplus::daemon::agent_mcp {

// Calls one agents.* operation on the daemon and returns its result payload.
using DaemonCall = std::function<QJsonObject(const QString& operation, const QJsonObject& payload)>;

// Answers one MCP JSON-RPC message from a provider. Notifications return an empty object.
[[nodiscard]] QJsonObject handleMessage(const QJsonObject& message, const DaemonCall& call);

// `ketplus_daemon --mcp-agents`: serves MCP on stdio for the conversation whose token is in
// KETPLUS_AGENT_TOKEN, through the daemon socket in KETPLUS_AGENT_SOCKET. Returns the exit code.
int run();

} // namespace ketplus::daemon::agent_mcp
