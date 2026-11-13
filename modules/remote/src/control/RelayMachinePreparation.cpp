#include "control/ControlV2Client.h"
#include "control/RelayMachineBackend.h"

#include <QDateTime>

namespace ketplus::remote {

void RelayMachineBackend::createSession(const QJsonObject& request, Callback callback) {
    if (request.value("prompt").toString().trimmed().isEmpty()) {
        callback({}, QStringLiteral("Enter a prompt before creating a conversation."));
        return;
    }
    worktreeCommand(QStringLiteral("session.create"), request, std::move(callback));
}

void RelayMachineBackend::worktreeCommand(const QString& kind, const QJsonObject& request,
                                          Callback callback) {
    if (hold(callback, [this, kind, request](Callback again) {
            worktreeCommand(kind, request, std::move(again));
        }))
        return;
    const auto repository = request.value("repository_id").toString();
    const auto worktreeId = request.value("worktree_id").toString();
    QJsonObject worktree;
    for (const auto& value : std::as_const(repositories_))
        if (value.toObject().value("repositoryId").toString() == repository)
            for (const auto& item : value.toObject().value("worktrees").toArray())
                if (item.toObject().value("worktreeId").toString() == worktreeId)
                    worktree = item.toObject();
    // Only a worktree the host vouched for (a verified conversation or its signed read).
    if (worktree.isEmpty() || worktree.value("workspaceId").toString().isEmpty()) {
        callback({}, QStringLiteral("Choose a verified worktree of this machine."));
        return;
    }
    QJsonObject plaintext;
    for (const auto* key : {"prompt", "options", "new_worktree", "client_request_id"})
        if (request.contains(QLatin1String(key)))
            plaintext.insert(QLatin1String(key), request.value(QLatin1String(key)));
    const QJsonObject intent{
        {"kind", kind},
        {"workspaceId", worktree.value("workspaceId")},
        {"machineId", relayMachineId_},
        {"repositoryId", repository},
        {"worktreeId", worktreeId},
        {"expectedCatalogRevision", worktree.value("catalogRevision").toInteger(0)},
        {"expectedLifecycleRevision", worktree.value("lifecycleRevision").toInteger(0)}};
    const auto generation = generation_;
    client_->prepareWorktreeCommand(
        intent, plaintext,
        guarded([this, generation, callback = std::move(callback)](const QJsonValue& value,
                                                                   const QString& error) mutable {
            if (generation != generation_) {
                callback({}, reason_);
                return;
            }
            if (!error.isEmpty()) {
                callback({}, error);
                return;
            }
            persistAndDeliver(OutboxRoute::Worktree, value.toObject(), std::move(callback));
        }));
}

void RelayMachineBackend::prepareSession(const QJsonObject& request, Callback callback) {
    if (hold(callback,
             [this, request](Callback again) { prepareSession(request, std::move(again)); }))
        return;
    if (!supports(Feature::SessionPreparation)) {
        callback({}, unsupportedReason(Feature::SessionPreparation));
        return;
    }
    if (request.value("client_request_id").toString().isEmpty() ||
        request.contains("new_worktree")) {
        callback({}, QStringLiteral("Choose an existing worktree and a stable attachment draft."));
        return;
    }
    const auto epoch = generation_;
    QString workspace;
    for (const auto& entry : std::as_const(repositories_))
        if (entry.toObject().value("repositoryId") == request.value("repository_id"))
            for (const auto& candidate : entry.toObject().value("worktrees").toArray())
                if (candidate.toObject().value("worktreeId") == request.value("worktree_id"))
                    workspace = candidate.toObject().value("workspaceId").toString();
    worktreeCommand(
        QStringLiteral("session.prepare"), request,
        [self = QPointer<RelayMachineBackend>(this), epoch, workspace,
         callback = std::move(callback)](const QJsonObject& result, const QString& error) {
            if (!self)
                return;
            if (epoch != self->generation_) {
                callback({}, self->reason_);
                return;
            }
            if (!error.isEmpty() || result.value("status") == "queued") {
                callback(result, error);
                return;
            }
            const auto id = result.value("session_id").toString();
            if (!result.value("ok").toBool() || id.isEmpty() ||
                result.value("session_generation").toInteger() != 1 ||
                result.value("expires_at_ms").toInteger() <= QDateTime::currentMSecsSinceEpoch()) {
                callback({}, result.value("error_message")
                                 .toString(QStringLiteral(
                                     "The host did not prepare an attachment draft.")));
                return;
            }
            // The result was authenticated with the paired host key. No phone-visible
            // conversation or event subscription is created before the first real turn.
            self->prepared_.insert(id, {{"id", id},
                                        {"sessionId", id},
                                        {"workspaceId", workspace},
                                        {"machineId", self->relayMachineId_},
                                        {"generation", 1},
                                        {"expires_at_ms", result.value("expires_at_ms")}});
            callback(result, {});
        });
}

void RelayMachineBackend::cancelPreparedSession(const QString& sessionId, qint64 generation,
                                                Callback callback) {
    if (!prepared_.contains(sessionId)) {
        callback({}, QStringLiteral("This attachment draft is no longer prepared."));
        return;
    }
    sessionCommand(QStringLiteral("session.prepare.cancel"), sessionId, generation, {},
                   std::move(callback));
}

} // namespace ketplus::remote
