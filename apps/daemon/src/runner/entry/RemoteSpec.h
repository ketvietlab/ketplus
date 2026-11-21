#pragma once

#include "protocol/Frames.h"

#include <QJsonObject>
#include <QString>

#include <optional>

// The job spec of a remote job, read from the execution of a process task once the checks every
// task goes through have passed: argv or shell_command, an absolute cwd and env_delta.
namespace ketplus::daemon::runner::entry {

namespace protocol = ketplus::runner::protocol;

// The invocation, environment, timeout and absolute cwd; the snapshot fields are left for the
// submit to fill. nullopt when the execution is not a process execution.
[[nodiscard]] std::optional<protocol::JobSpec> jobSpecOf(const QJsonObject& execution,
                                                         qint64 timeoutMs);

// Empty when a runner can take the execution; otherwise why not, refused invalid_request before
// any task exists. The field rules are those of the job.spec frame, with the largest snapshot
// fields the submit can add, and `cwd` relative to its worktree.
[[nodiscard]] QString remoteSpecProblem(const QJsonObject& execution, qint64 timeoutMs);

} // namespace ketplus::daemon::runner::entry
