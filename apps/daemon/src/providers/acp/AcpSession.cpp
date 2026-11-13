#include "BrowserToolLaunch.h"
#include "ComputerToolLaunch.h"
#include "providers/acp/AcpAdapter.h"

#include <QCoreApplication>

namespace ketplus::daemon {

void AcpAdapter::ensureAcpSession(const QString& ketplusSessionId, const QString& cwd,
                                  const QString& resume, const QString& model, Callback callback) {
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
    const QJsonArray browserServers = browser_tools::acpMcpServers(launch);
    const bool browserInjected = !browserServers.isEmpty();
    // ADR 0030: the computer front is bound to this conversation by its token.
    const auto computer = computer_tools::launchFromUserSettings(
        ketplusSessionId, QCoreApplication::applicationDirPath());
    QJsonArray servers = computer_tools::withAcpMcpServer(browserServers, computer);
    // ADR 0027: the conversation's own ketplus_agents server, when SessionHost supplied one.
    const QJsonObject agentServer = agentServers_.value(ketplusSessionId);
    if (!agentServer.isEmpty())
        servers.append(agentServer);
    const auto bind = [this, ketplusSessionId, callback = std::move(callback), model,
                       browserInjected, agentsInjected = !agentServer.isEmpty(),
                       computerInjected = computer.has_value(),
                       turnEndFile](const QJsonObject& result, const QString& error) {
        if (!error.isEmpty()) {
            if (!turnEndFile.isEmpty())
                browser_tools::signalTurnEnd(turnEndFile);
            callback({}, error);
            return;
        }
        const QString acpId = result.value(QStringLiteral("sessionId")).toString();
        if (acpId.isEmpty()) {
            if (!turnEndFile.isEmpty())
                browser_tools::signalTurnEnd(turnEndFile);
            callback({}, QStringLiteral("%1 ACP returned no session id.").arg(label_));
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
        if (computerInjected)
            computerSessions_.insert(ketplusSessionId);
        else
            computerSessions_.remove(ketplusSessionId);
        ketplusByAcp_.insert(acpId, ketplusSessionId);
        applySessionModel(acpId, model, result, [this, callback, acpId, ketplusSessionId] {
            const QString mode =
                sessionPermissionMode(permissionBySession_.value(ketplusSessionId));
            if (mode.isEmpty()) {
                callback(QJsonObject{{QStringLiteral("sessionId"), acpId}}, {});
                return;
            }
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

void AcpAdapter::applyConfigOptions(const QString& acpSessionId,
                                    QList<QPair<QString, QString>> options,
                                    std::function<void()> done) {
    if (options.isEmpty()) {
        done();
        return;
    }
    const QPair<QString, QString> next = options.takeFirst();
    sendCall(
        QStringLiteral("session/set_config_option"),
        {{QStringLiteral("sessionId"), acpSessionId},
         {QStringLiteral("configId"), next.first},
         {QStringLiteral("value"), next.second}},
        [this, acpSessionId, options, done = std::move(done)](const QJsonObject&, const QString&) {
            applyConfigOptions(acpSessionId, options, done);
        });
}

void AcpAdapter::handlePermissionRequest(const QJsonValue& requestId, const QJsonObject& params) {
    if (strictPermissionRequests()) {
        handleStrictPermission(requestId, params);
        return;
    }
    const QString acpId = params.value(QStringLiteral("sessionId")).toString();
    const QString sessionId = ketplusByAcp_.value(acpId);
    if (sessionId.isEmpty()) {
        respond(requestId, {{QStringLiteral("allow"), false}});
        return;
    }
    permissionOptionsByRequest_.insert(requestKey(requestId), params.value("options").toArray());
    if (permissionBySession_.value(sessionId) == QStringLiteral("danger-full-access")) {
        respond(requestId, {{QStringLiteral("allow"), true}});
        return;
    }
    const QJsonObject toolCall = params.value(QStringLiteral("toolCall")).toObject();
    QJsonObject input = toolCall.value(QStringLiteral("rawInput")).toObject();
    if (autoApprovePermission(sessionId, toolCall)) {
        respond(requestId, {{QStringLiteral("allow"), true}});
        return;
    }
    if (input.isEmpty())
        input = toolCall;
    emit requestReceived(
        requestId, QStringLiteral("permission.request"),
        {{QStringLiteral("sessionId"), sessionId},
         {QStringLiteral("tool"), toolCall.value(QStringLiteral("title"))
                                      .toString(toolCall.value(QStringLiteral("kind")).toString())},
         {QStringLiteral("input"), input}});
}

} // namespace ketplus::daemon
