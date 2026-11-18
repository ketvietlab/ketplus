#include "agents/AgentToolLaunch.h"

#include "CodexBrowserTools.h"

#include <QJsonArray>

namespace ketplus::daemon::agent_tools {

QJsonObject claudeServer(const Launch& launch) {
    return {{QStringLiteral("type"), QStringLiteral("stdio")},
            {QStringLiteral("command"), launch.command},
            {QStringLiteral("args"), QJsonArray{QLatin1String(mcpArgument)}},
            {QStringLiteral("env"),
             QJsonObject{{QLatin1String(socketVariable), launch.socket},
                         {QLatin1String(tokenVariable), launch.token},
                         {QLatin1String(mutationVariable), launch.mutationCredential},
                         {QLatin1String(epochVariable), launch.transportEpoch}}}};
}

QJsonObject acpServer(const Launch& launch) {
    return {{QStringLiteral("name"), QLatin1String(serverName)},
            {QStringLiteral("command"), launch.command},
            {QStringLiteral("args"), QJsonArray{QLatin1String(mcpArgument)}},
            {QStringLiteral("env"),
             QJsonArray{QJsonObject{{QStringLiteral("name"), QLatin1String(socketVariable)},
                                    {QStringLiteral("value"), launch.socket}},
                        QJsonObject{{QStringLiteral("name"), QLatin1String(tokenVariable)},
                                    {QStringLiteral("value"), launch.token}},
                        QJsonObject{{QStringLiteral("name"), QLatin1String(mutationVariable)},
                                    {QStringLiteral("value"), launch.mutationCredential}},
                        QJsonObject{{QStringLiteral("name"), QLatin1String(epochVariable)},
                                    {QStringLiteral("value"), launch.transportEpoch}}}}};
}

QJsonObject codexConfig(const Launch& launch) {
    const QString prefix = QStringLiteral("mcp_servers.%1.").arg(QLatin1String(serverName));
    return {{prefix + QStringLiteral("command"), launch.command},
            {prefix + QStringLiteral("args"), QJsonArray{QLatin1String(mcpArgument)}},
            {prefix + QStringLiteral("env"),
             QJsonObject{{QLatin1String(socketVariable), launch.socket},
                         {QLatin1String(tokenVariable), launch.token},
                         {QLatin1String(mutationVariable), launch.mutationCredential},
                         {QLatin1String(epochVariable), launch.transportEpoch}}},
            // Agent tools run without a per-call prompt, as the browser tools do.
            {prefix + QStringLiteral("default_tools_approval_mode"),
             QLatin1String(browser_tools::defaultToolsApprovalMode)}};
}

} // namespace ketplus::daemon::agent_tools
