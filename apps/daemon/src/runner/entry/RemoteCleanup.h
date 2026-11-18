#pragma once

#include "runner/entry/RemoteJobPort.h"

#include <QJsonObject>
#include <QString>
#include <optional>

namespace ketplus::daemon::runner::entry {

// session.task.resolve_cleanup on a remote job: nullopt when the task has no remote record. As
// on the local path, the request's revision must be the task's current one (revision_conflict).
[[nodiscard]] inline std::optional<QString>
resolveRemoteCleanup(RemoteJobPort& port, const QString& taskId, const QJsonObject& payload,
                     const QJsonObject& authorized, const QString& userClientId) {
    if (!port.hasRecord(taskId))
        return std::nullopt;
    const qint64 current = authorized.value(QStringLiteral("task"))
                               .toObject()
                               .value(QStringLiteral("revision"))
                               .toInteger();
    if (payload.value(QStringLiteral("revision")).toInteger(-1) != current)
        return QStringLiteral("revision_conflict");
    return port.resolveCleanup(taskId, userClientId);
}

} // namespace ketplus::daemon::runner::entry
