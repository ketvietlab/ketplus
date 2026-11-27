#include "runner/remote/TaskSnapshots.h"

#include "runner/sync/SnapshotKeys.h"

namespace ketplus::daemon::runner::remote {

TaskSnapshots::TaskSnapshots(peer::JobSpecSource& specs, Owner owner, Secret secret)
    : specs_(specs), owner_(std::move(owner)), secret_(std::move(secret)) {}

void TaskSnapshots::put(const QString& taskId, TaskSnapshot snapshot) {
    snapshots_.insert(taskId, std::move(snapshot));
}

void TaskSnapshots::remove(const QString& taskId) { snapshots_.remove(taskId); }

std::optional<TaskSnapshot> TaskSnapshots::find(const QString& taskId) const {
    const auto found = snapshots_.constFind(taskId);
    return found == snapshots_.cend() ? std::nullopt : std::optional(*found);
}

std::optional<TaskSnapshots::Keys> TaskSnapshots::keys(const TaskSnapshot& snapshot,
                                                       const QString& runnerId) {
    const std::optional<QByteArray> secret = secret_ ? secret_() : std::nullopt;
    if (!secret || runnerId.isEmpty())
        return std::nullopt;
    return Keys{sync::repositoryKey(*secret, runnerId, snapshot.repositoryId),
                sync::workspaceKey(*secret, runnerId, snapshot.repositoryId, snapshot.worktreeId)};
}

std::optional<peer::protocol::JobSpec> TaskSnapshots::spec(const QString& jobId,
                                                           const qint64 attempt) {
    const std::optional<JobOwner> owner = owner_ ? owner_(jobId) : std::nullopt;
    if (!owner)
        return std::nullopt;
    const auto snapshot = snapshots_.constFind(owner->taskId);
    if (snapshot == snapshots_.cend())
        return std::nullopt;
    std::optional<peer::protocol::JobSpec> spec = specs_.spec(jobId, attempt);
    const std::optional<Keys> found = keys(*snapshot, owner->runnerId);
    if (!spec || !found)
        return std::nullopt;
    spec->repositoryKey = found->repositoryKey;
    spec->workspaceKey = found->workspaceKey;
    spec->snapshotId = snapshot->snapshotId;
    return spec;
}

std::optional<SnapshotPlan> TaskSnapshots::plan(const QString& jobId, const QString& runnerId) {
    const std::optional<JobOwner> owner = owner_ ? owner_(jobId) : std::nullopt;
    if (!owner || owner->runnerId != runnerId)
        return std::nullopt;
    const auto snapshot = snapshots_.constFind(owner->taskId);
    if (snapshot == snapshots_.cend())
        return std::nullopt;
    const std::optional<Keys> found = keys(*snapshot, runnerId);
    if (!found)
        return std::nullopt;
    SnapshotPlan plan;
    plan.taskId = owner->taskId;
    plan.storePath = snapshot->storePath;
    plan.job.jobId = jobId;
    plan.job.runnerId = runnerId;
    plan.job.workspaceKey = found->workspaceKey;
    plan.job.snapshotId = snapshot->snapshotId;
    plan.job.warnings = snapshot->warnings;
    return plan;
}

} // namespace ketplus::daemon::runner::remote
