#include "runtime/ManagedTaskBridge.h"
#include "runtime/SessionHost.h"

#include <QSet>

namespace ketplus::daemon {
void SessionHost::prepareManagedTaskOrigin(Session& session, const QJsonObject& options) {
    session.mutationOrigin.revoke();
    if (!agentSocket_.isEmpty() && !agentExecutable_.isEmpty())
        session.mutationOrigin.prepare(
            session.stored.id, session.stored.generation, session.stored.currentTurnSequence,
            options.value("execution_policy").toString(), options.value("review_guard").toBool());
}

bool SessionHost::isManagedTaskEpoch(const Session& session, const QString& epoch) const {
    return epoch.isEmpty() || session.mutationOrigin.bootstrap().transportEpoch == epoch;
}

bool SessionHost::acknowledgeManagedTaskOrigin(Session& session, const QString& epoch,
                                               const QJsonObject& result) {
    if (epoch.isEmpty())
        return true;
    if (result.value("managedTransportEpoch") == epoch && session.mutationOrigin.activate(epoch))
        return true;
    failTurnStart(
        session,
        "mutation_refresh_unavailable: The provider did not acknowledge its origin epoch.");
    return false;
}

bool SessionHost::isManagedTaskCommand(const QString& type) {
    static const QSet<QString> types{"session.task.start",  "session.task.list",
                                     "session.task.get",    "session.task.stop",
                                     "session.task.output", "session.task.resolve_cleanup"};
    return types.contains(type);
}

void SessionHost::initializeManagedTasks() {
    managedTasks_ = new ManagedTaskBridge(this);
    QString error;
    if (!managedTasks_->initialize(&error))
        qWarning("Managed background tasks are unavailable: %s", qUtf8Printable(error));
}

void SessionHost::managedTaskCommand(const QJsonObject& request, const QString& clientId,
                                     CommandCallback done, bool agentCaller,
                                     std::optional<AgentMutationGrant> origin) {
    if (!managedTasks_) {
        done({{"ok", false},
              {"error_code", "registry_unavailable"},
              {"error_message", "Managed background tasks are unavailable."}});
        return;
    }
    managedTasks_->command(request, clientId, std::move(done), agentCaller, std::move(origin));
}

bool SessionHost::managedTaskPermission(const QJsonValue& requestId, const QJsonObject& result) {
    return managedTasks_ && managedTasks_->permission(requestId, result);
}

void SessionHost::holdManagedTaskWake(const QString& id, qint64 generation, const QString& reason) {
    if (managedTasks_)
        managedTasks_->hold({id, generation, {}}, reason);
}

void SessionHost::resumeManagedTaskWake(const QString& id, qint64 generation) {
    if (managedTasks_)
        managedTasks_->resume({id, generation, {}});
}

QJsonObject SessionHost::managedTaskSnapshot(const QString& id, qint64 generation) const {
    return managedTasks_ ? managedTasks_->snapshot({id, generation, {}}) : QJsonObject{};
}
} // namespace ketplus::daemon
