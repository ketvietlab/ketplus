#include "DaemonClient.h"
#include "agent/CodexAppServer.h"

#include <QSet>

namespace ketplus {
QVariantMap CodexAppServer::backgroundTaskAccess() const {
    return {{"connected", daemon_ && daemon_->isReady()},
            {"canRead", daemon_ && daemon_->hasCapability("session.read")},
            {"canControl", daemon_ && daemon_->hasCapability("session.control")},
            {"canResolve", daemon_ && daemon_->hasCapability("session.control")}};
}

void CodexAppServer::callDaemonBackgroundTask(const QString& method, const QJsonObject& params,
                                              ResponseCallback callback) {
    const auto action = method.mid(5);
    static const QSet<QString> operations{"list", "get", "stop", "output", "resolve_cleanup"};
    const auto sessionId = params.value("threadId").toString();
    const auto found = daemonSessions_.constFind(sessionId);
    const bool mutation = action == "stop" || action == "resolve_cleanup";
    if (!operations.contains(action) || found == daemonSessions_.cend()) {
        callback({}, QStringLiteral("Unknown background task operation or conversation."));
        return;
    }
    // Reject new mutations while disconnected. The server still authorizes
    // every operation; permissions are negotiated, never inferred from provider.
    if (!daemon_->isReady() ||
        !daemon_->hasCapability(mutation ? "session.control" : "session.read")) {
        callback({}, QStringLiteral(
                         "Background task access is unavailable. Reconnect or check permissions."));
        return;
    }
    QJsonObject payload;
    if (params.contains("taskId"))
        payload.insert("task_id", params.value("taskId"));
    for (const auto& key : {"cursor", "revision", "limit"})
        if (params.contains(key))
            payload.insert(key, params.value(key));
    const auto generation = found->generation;
    daemon_->call(
        "session.task." + action, payload, sessionId, generation,
        [this, sessionId, generation, callback = std::move(callback)](const QJsonObject& result,
                                                                      const QString& error) {
            const auto current = daemonSessions_.constFind(sessionId);
            if (current == daemonSessions_.cend() || current->generation != generation) {
                callback({}, QStringLiteral(
                                 "The background task owner changed. Refresh the conversation."));
                return;
            }
            callback(result, error);
        });
}
} // namespace ketplus
