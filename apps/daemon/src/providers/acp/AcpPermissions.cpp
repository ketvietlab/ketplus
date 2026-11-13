#include "providers/acp/AcpAdapter.h"

namespace ketplus::daemon {
void AcpAdapter::sendPermissionOutcome(const QJsonValue& requestId, const QString& optionId) {
    QJsonObject outcome{{"outcome", "cancelled"}};
    if (!optionId.isEmpty())
        outcome = {{"outcome", "selected"}, {"optionId", optionId}};
    sendObject(
        {{"jsonrpc", "2.0"}, {"id", requestId}, {"result", QJsonObject{{"outcome", outcome}}}});
}

void AcpAdapter::handleStrictPermission(const QJsonValue& requestId, const QJsonObject& params) {
    // A duplicate outstanding wire id cannot identify a second request safely.
    for (const auto& pending : permissions_)
        if (requestKey(pending.providerId) == requestKey(requestId))
            return;
    const QString acpId = params.value("sessionId").toString();
    const QString sessionId = ketplusByAcp_.value(acpId);
    const auto turn = turns_.constFind(sessionId);
    if (turn == turns_.cend() || turn->acpSessionId != acpId || turn->interrupted ||
        permissions_.size() >= 128) {
        sendPermissionOutcome(requestId);
        return;
    }
    const QJsonArray options = params.value("options").toArray();
    if (options.isEmpty() || options.size() > 256) {
        sendPermissionOutcome(requestId);
        return;
    }
    QSet<QString> ids;
    for (const auto& value : options) {
        const QString id = value.toObject().value("optionId").toString();
        if (id.isEmpty() || ids.contains(id)) {
            sendPermissionOutcome(requestId);
            return;
        }
        ids.insert(id);
    }
    // Monotonic host IDs survive process restarts. A late broker answer cannot hit a
    // replacement turn even when the provider restarts its own ids at the same value.
    const QString hostId = QStringLiteral("ketplus-acp-permission-%1").arg(nextPermissionNumber_++);
    permissions_.insert(requestKey(hostId), {requestId, sessionId, turn->turnId, options});
    const QJsonObject tool = params.value("toolCall").toObject();
    if (autoApprovePermission(sessionId, tool)) {
        respondStrictPermission(hostId, {{"allow", true}});
        return;
    }
    QJsonObject input = tool.value("rawInput").toObject();
    if (input.isEmpty())
        input = tool;
    emit requestReceived(hostId, QStringLiteral("permission.request"),
                         {{"sessionId", sessionId},
                          {"tool", tool.value("title").toString(tool.value("kind").toString())},
                          {"input", input}});
}

void AcpAdapter::respondStrictPermission(const QJsonValue& requestId, const QJsonObject& result) {
    const auto it = permissions_.find(requestKey(requestId));
    if (it == permissions_.end())
        return;
    const PermissionRequest pending = it.value();
    permissions_.erase(it);
    const auto turn = turns_.constFind(pending.sessionId);
    QString optionId;
    if (turn != turns_.cend() && turn->turnId == pending.turnId && !turn->interrupted &&
        !result.value("cancelled").toBool()) {
        const bool allow = result.value("allow").toBool() || result.value("decision") == "accept";
        // KetPlus's approval card is a one-time decision. Never promote it to an
        // always rule or invent an option when the provider did not offer one.
        const QString kind = allow ? QStringLiteral("allow_once") : QStringLiteral("reject_once");
        for (const auto& value : pending.options)
            if (value.toObject().value("kind") == kind) {
                optionId = value.toObject().value("optionId").toString();
                break;
            }
    }
    sendPermissionOutcome(pending.providerId, optionId);
}

void AcpAdapter::cancelPermissions(const QString& sessionId) {
    const auto keys = permissions_.keys();
    for (const auto& key : keys) {
        const auto pending = permissions_.value(key);
        if (!sessionId.isEmpty() && pending.sessionId != sessionId)
            continue;
        permissions_.remove(key);
        sendPermissionOutcome(pending.providerId);
    }
}
} // namespace ketplus::daemon
