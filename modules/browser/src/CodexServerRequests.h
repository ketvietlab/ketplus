#pragma once

// Response shapes for Codex app-server -> client requests (codex-cli 0.154.0,
// `codex app-server generate-json-schema --experimental`). Shared by the daemon SessionHost and
// the desktop direct-mode CodexAppServer so both answer with the shape Codex expects.

#include "CodexBrowserTools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ketplus::codex_requests {

inline const QString mcpElicitationMethod = QStringLiteral("mcpServer/elicitation/request");

// McpServerElicitationRequestResponse: `action` is accept | decline | cancel; `content` carries
// the form values for an accepted form. A tool-call approval form has no fields.
inline QJsonObject mcpElicitationResult(const bool accepted) {
    if (!accepted)
        return {{QStringLiteral("action"), QStringLiteral("decline")}};
    return {{QStringLiteral("action"), QStringLiteral("accept")},
            {QStringLiteral("content"), QJsonObject{}}};
}

// Codex asks before an MCP tool call with a form elicitation tagged
// `_meta.codex_approval_kind = "mcp_tool_call"`. Only those can be answered by an accept/decline
// card. Untagged forms (even field-less ones), URL and user-verification elicitations come from
// the MCP server itself and are declined.
inline bool isMcpToolApproval(const QJsonObject& params) {
    if (params.value(QStringLiteral("mode")).toString() != QStringLiteral("form"))
        return false;
    return params.value(QStringLiteral("_meta"))
               .toObject()
               .value(QStringLiteral("codex_approval_kind"))
               .toString() == QStringLiteral("mcp_tool_call");
}

// True when the Codex process was launched with KetPlus' own `mcp_servers.ketplus_browser.*`
// arguments. Without them a `ketplus_browser` server comes from the user's Codex config.
inline bool launchedWithBrowserTools(const QStringList& launchArguments) {
    const QString prefix =
        QStringLiteral("mcp_servers.%1.").arg(QLatin1String(browser_tools::serverName));
    for (const QString& argument : launchArguments) {
        if (argument.startsWith(prefix))
            return true;
    }
    return false;
}

// Browser tools never prompt (ADR 0008), but only for the server KetPlus injected at launch.
inline bool isInjectedBrowserToolApproval(const QJsonObject& params,
                                          const QStringList& launchArguments) {
    return isMcpToolApproval(params) &&
           params.value(QStringLiteral("serverName")).toString() ==
               QLatin1String(browser_tools::serverName) &&
           launchedWithBrowserTools(launchArguments);
}

// Metadata for the approval card. Tool arguments are not copied: `message` is Codex's own
// summary ("Allow <server> to run tool ...?").
inline QJsonObject mcpToolApprovalDetails(const QJsonObject& params) {
    const QJsonObject meta = params.value(QStringLiteral("_meta")).toObject();
    const QString server = params.value(QStringLiteral("serverName")).toString();
    QString reason = params.value(QStringLiteral("message")).toString();
    if (reason.isEmpty())
        reason = QStringLiteral("Allow the %1 MCP server to run a tool?").arg(server);
    QJsonObject details{{QStringLiteral("request_type"), QStringLiteral("mcp_tool_call")},
                        {QStringLiteral("server_name"), server},
                        {QStringLiteral("reason"), reason}};
    const QString title = meta.value(QStringLiteral("tool_title")).toString();
    if (!title.isEmpty())
        details.insert(QStringLiteral("tool_title"), title);
    return details;
}

// A refusal in the shape each request method expects.
inline QJsonObject declinedResult(const QString& method) {
    if (method == mcpElicitationMethod)
        return mcpElicitationResult(false);
    if (method == QStringLiteral("item/permissions/requestApproval"))
        return {{QStringLiteral("permissions"), QJsonObject{}}}; // grants nothing
    if (method == QStringLiteral("item/tool/call"))
        return {{QStringLiteral("contentItems"), QJsonArray{}}, {QStringLiteral("success"), false}};
    return {{QStringLiteral("decision"), QStringLiteral("decline")}};
}

} // namespace ketplus::codex_requests
