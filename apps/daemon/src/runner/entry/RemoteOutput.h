#pragma once

#include "storage/BackgroundTaskOutput.h"
#include "storage/RemoteJobRows.h"

#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>

#include <optional>

namespace ketplus::daemon::runner::entry {

// The output page of a task whose remote job started: the remote jobs keep what its runner sent
// in the registry, where no executor writes. Empty for any other task. `authorized` is the
// registry's answer for the task, as the output command read it.
inline std::optional<QJsonObject> remoteOutput(const QSqlDatabase& database, const QString& taskId,
                                               const QString& cursor,
                                               const QJsonObject& authorized) {
    if (remote_job_rows::phaseOf(database, taskId) != remote_job_rows::Phase::Started)
        return std::nullopt;
    const QJsonObject task = authorized.value(QStringLiteral("task")).toObject();
    const BackgroundOutputPage page = BackgroundTaskOutput(database).read(
        taskId, cursor, BackgroundTaskOutput::maximumPageBytes, true,
        task.value(QStringLiteral("ended_at")).isDouble());
    if (!page.error.isEmpty())
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error_code"), QStringLiteral("output_unavailable")},
                           {QStringLiteral("error_message"), page.error}};
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("output"), QString::fromUtf8(page.bytes)},
                       {QStringLiteral("next_cursor"), page.nextCursor},
                       {QStringLiteral("total_bytes"), page.totalBytes},
                       {QStringLiteral("truncated"),
                        page.truncated || task.value(QStringLiteral("output_truncated")).toBool()}};
}

} // namespace ketplus::daemon::runner::entry
