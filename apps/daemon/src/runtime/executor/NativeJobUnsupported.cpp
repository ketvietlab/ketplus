#include <QtGlobal>
#if defined(Q_OS_WIN)
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobClient.h"

// Managed background process jobs rest on a POSIX supervisor (process groups, cgroups,
// kernel-bound signal targets). The Windows daemon does not offer them: ManagedTaskRuntime
// refuses a "process" task before it reaches this client with "containment_unavailable".
// These definitions exist so the daemon links, and every one of them fails closed.
namespace ketplus::daemon::executor {
namespace {
const QString unsupported = QStringLiteral("Managed background jobs are not available on Windows.");
}

QString linuxJobUnit(const QString&, const QString&) { return {}; }
QString linuxBootId() { return {}; }

NativeJobClient::NativeJobClient(QString executable, QString directory, QString taskId,
                                 QString lease, QObject* parent)
    : QObject(parent), executable_(std::move(executable)), directory_(std::move(directory)),
      taskId_(std::move(taskId)), lease_(std::move(lease)) {}

NativeJobClient::~NativeJobClient() = default;

bool NativeJobClient::start(const QJsonObject&, qint64, CommitHandshake, Event, QString* error) {
    if (error)
        *error = unsupported;
    return false;
}

bool NativeJobClient::reconnect(Event, QString* error) {
    if (error)
        *error = unsupported;
    return false;
}

void NativeJobClient::stop(const QString&) {}
void NativeJobClient::acknowledge() {}

QJsonObject NativeJobClient::receipt(QString* error) const {
    if (error)
        *error = unsupported;
    return {};
}
} // namespace ketplus::daemon::executor
#endif
