#pragma once

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include <optional>

// The registry rows of remote jobs. A task created with runs_on has a remote row from the
// start: reserved (revision 0, runs_on and timeout only) until its record is first stored, then
// started. A remote task never runs locally.
namespace ketplus::daemon::remote_job_rows {

enum class Phase { Local, Reserved, Started };

// Adds the remote table and its cleanup trigger when missing; changes nothing else.
[[nodiscard]] bool initialize(QSqlDatabase database, QString* error);
// nullopt when the row cannot be read.
[[nodiscard]] std::optional<Phase> phaseOf(QSqlDatabase database, const QString& taskId);
// Inside the caller's transaction.
[[nodiscard]] bool reserve(QSqlDatabase database, const QString& taskId, const QStringList& runsOn,
                           qint64 timeoutMs, QString* error);
// The spec fingerprint of a task created with runs_on; a local task keeps `fingerprint`.
[[nodiscard]] QByteArray fingerprint(const QByteArray& fingerprint,
                                     const std::optional<QStringList>& runsOn, qint64 timeoutMs);

// An error code when a local execution step must refuse `taskId`, empty when it is local.
[[nodiscard]] QString localExecutionRefusal(QSqlDatabase database, const QString& taskId);
// An error code when the registry's own change to `state` must refuse `taskId`: a reserved
// task cannot start or run, a started one only changes through its record.
[[nodiscard]] QString changeRefusal(QSqlDatabase database, const QString& taskId,
                                    const QString& state);

} // namespace ketplus::daemon::remote_job_rows
