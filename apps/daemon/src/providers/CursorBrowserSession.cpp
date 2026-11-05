#include "BrowserToolLaunch.h"
#include "providers/CursorAdapter.h"

#include <QCoreApplication>

namespace ketplus::daemon {

void CursorAdapter::ensureAcpSession(const QString& ketplusSessionId, const QString& cwd,
                                     const QString& resume, const QString& model,
                                     Callback callback) {
    auto launch =
        browser_tools::browserLaunchFromUserSettings(QCoreApplication::applicationDirPath());
    QString turnEndFile;
    if (launch) {
        // The agent keeps this server across turns; finishTurn signals it to close what the
        // finished turn left open.
        turnEndFile = browser_tools::turnEndSignalFile(
            launch->env.value(QStringLiteral("KETPLUS_BROWSER_ARTIFACTS")), ketplusSessionId);
        if (!turnEndFile.isEmpty())
            launch->env.insert(QLatin1String(browser_tools::turnEndFileVariable), turnEndFile);
    }
    QJsonArray servers = browser_tools::acpMcpServers(launch);
    const bool browserInjected = !servers.isEmpty();
    // ADR 0027: the conversation's own ketplus_agents server, when SessionHost supplied one.
    const QJsonObject agentServer = agentServers_.value(ketplusSessionId);
    if (!agentServer.isEmpty())
        servers.append(agentServer);
    const auto bind = [this, ketplusSessionId, callback = std::move(callback), model,
                       browserInjected, agentsInjected = !agentServer.isEmpty(),
                       turnEndFile](const QJsonObject& result, const QString& error) {
        if (!error.isEmpty()) {
            callback({}, error);
            return;
        }
        const QString acpId = result.value(QStringLiteral("sessionId")).toString();
        if (acpId.isEmpty()) {
            callback({}, QStringLiteral("Cursor ACP returned no session id."));
            return;
        }
        if (!acpSessionByKetplus_.value(ketplusSessionId).isEmpty())
            ketplusByAcp_.remove(acpSessionByKetplus_.value(ketplusSessionId));
        acpSessionByKetplus_.insert(ketplusSessionId, acpId);
        if (browserInjected)
            browserSessions_.insert(ketplusSessionId, turnEndFile);
        else
            browserSessions_.remove(ketplusSessionId);
        if (agentsInjected)
            agentSessions_.insert(ketplusSessionId);
        else
            agentSessions_.remove(ketplusSessionId);
        ketplusByAcp_.insert(acpId, ketplusSessionId);
        applySessionModel(
            acpId, model, result.value(QStringLiteral("configOptions")).toArray(),
            [this, callback, acpId, ketplusSessionId] {
                const QString mode = permissionMode(permissionBySession_.value(ketplusSessionId));
                sendCall(QStringLiteral("session/set_mode"),
                         {{QStringLiteral("sessionId"), acpId}, {QStringLiteral("modeId"), mode}},
                         [callback, acpId](const QJsonObject&, const QString&) {
                             callback(QJsonObject{{QStringLiteral("sessionId"), acpId}}, {});
                         });
            });
    };

    const QString existing =
        resume.isEmpty() ? acpSessionByKetplus_.value(ketplusSessionId) : resume;
    // An agent without loadSession starts a new ACP session; the journal keeps the transcript.
    if (!existing.isEmpty() && supports(compat::capability::threadResume)) {
        sendCall(QStringLiteral("session/load"),
                 {{QStringLiteral("sessionId"), existing},
                  {QStringLiteral("cwd"), cwd},
                  {QStringLiteral("mcpServers"), servers}},
                 [this, cwd, servers, existing, bind](QJsonObject result, const QString& error) {
                     if (error.isEmpty()) {
                         // ACP load succeeds with an empty object; the requested ID survives.
                         result.insert(QStringLiteral("sessionId"), existing);
                         bind(result, {});
                         return;
                     }
                     sendCall(
                         QStringLiteral("session/new"),
                         {{QStringLiteral("cwd"), cwd}, {QStringLiteral("mcpServers"), servers}},
                         bind);
                 });
        return;
    }
    sendCall(QStringLiteral("session/new"),
             {{QStringLiteral("cwd"), cwd}, {QStringLiteral("mcpServers"), servers}}, bind);
}

} // namespace ketplus::daemon
