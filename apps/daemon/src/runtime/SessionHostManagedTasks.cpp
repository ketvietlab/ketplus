#include "runtime/ManagedTaskBridge.h"
#include "runtime/SessionHost.h"

#include <QSet>

namespace ketplus::daemon {
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
                                     CommandCallback done, bool agentCaller) {
    if (!managedTasks_) {
        done({{"ok", false},
              {"error_code", "registry_unavailable"},
              {"error_message", "Managed background tasks are unavailable."}});
        return;
    }
    managedTasks_->command(request, clientId, std::move(done), agentCaller);
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
