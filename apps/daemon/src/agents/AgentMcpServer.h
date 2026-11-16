#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace ketplus::daemon::agent_mcp {

// Calls one agents.* or session.task.* operation using the bound agent token.
using DaemonCall = std::function<QJsonObject(const QString& operation, const QJsonObject& payload)>;
// Captures one bootstrap secret once; never consults a later turn's current token.
[[nodiscard]] DaemonCall withMutationCredential(DaemonCall call, QString credential);

// Answers one MCP JSON-RPC message from a provider. Notifications return an empty object.
[[nodiscard]] QJsonObject handleMessage(const QJsonObject& message, const DaemonCall& call,
                                        bool advertiseMutation = true);

// `ketplus_daemon --mcp-agents`: serves MCP on stdio for the conversation whose token is in
// KETPLUS_AGENT_TOKEN, through the daemon socket in KETPLUS_AGENT_SOCKET. Returns the exit code.
int run();

} // namespace ketplus::daemon::agent_mcp
