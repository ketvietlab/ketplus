#pragma once

#include <QJsonObject>
#include <QString>

namespace ketplus::daemon::agent_tools {

// ADR 0027: the ketplus_agents MCP server is the daemon executable in its MCP mode. It reaches
// the daemon's local socket with one conversation's capability token.
inline constexpr auto serverName = "ketplus_agents";
inline constexpr auto mcpArgument = "--mcp-agents";
inline constexpr auto socketVariable = "KETPLUS_AGENT_SOCKET";
inline constexpr auto tokenVariable = "KETPLUS_AGENT_TOKEN";

struct Launch final {
    QString command;
    QString socket;
    QString token;
};

// Claude SDK `mcpServers` entry.
[[nodiscard]] QJsonObject claudeServer(const Launch& launch);
// ACP `mcpServers` element.
[[nodiscard]] QJsonObject acpServer(const Launch& launch);
// Codex per-thread `config` entries. Dotted keys add this server beside the process-level ones;
// a nested `mcp_servers` object would replace them (ketplus_browser included).
[[nodiscard]] QJsonObject codexConfig(const Launch& launch);

} // namespace ketplus::daemon::agent_tools
