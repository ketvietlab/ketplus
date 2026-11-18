#pragma once

#include "session/SessionPorts.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

// What the runner asks of the executor, built without touching a process: the execution of a
// launch request, the mounts that hide the runner from it, and the command's end from the
// executor's result.
namespace ketplus::runner::exec {

// The private launch data the executor takes, at most this size together with the runner's
// own environment.
inline constexpr qsizetype maxLaunchBytes = 192 * 1024;

struct PreparedLaunch {
    QJsonObject execution; // argv or shell_command, cwd, env_delta, and mounts when planned
    QString error;         // [a-z0-9_]: why nothing may run
};

struct MountPlan {
    QStringList hidden; // replaced by empty file systems, in this order
    QString workspace;  // then mounted again, read-write, at its own path
    QString tmp;        // mounted on /tmp
    QString error;
};

// The execution of `request`. The command's environment becomes exactly
// `request.environment`: every variable of `runnerEnvironment` it lacks is removed. `cwd` is
// relative to the workspace and must stay inside it once symbolic links are resolved. With
// `mounts`, the execution carries them as `mounts` {hidden, workspace, tmp}.
[[nodiscard]] PreparedLaunch prepareLaunch(const session::LaunchRequest& request,
                                           const QMap<QString, QString>& runnerEnvironment,
                                           const MountPlan* mounts = nullptr);

// The mounts of a job: the runner's state, work and user runtime directories are hidden; the
// job's workspace and its own tmp directory, both inside the work directory and apart from each
// other, are visible again.
[[nodiscard]] MountPlan planMounts(const QString& stateDirectory, const QString& workDirectory,
                                   const QString& runtimeDirectory, const QString& workspace,
                                   const QString& tmp);

// The executor's task id for a job, and the name of its directory: the first 32 hex digits of
// the SHA-256 of `jobId`, '-', and `attempt`. Empty without a job.
[[nodiscard]] QString executorTaskId(const QString& jobId, qint64 attempt);

// How a command ended, from the executor's final result.
[[nodiscard]] session::CommandExit commandExit(const QJsonObject& result);

} // namespace ketplus::runner::exec
