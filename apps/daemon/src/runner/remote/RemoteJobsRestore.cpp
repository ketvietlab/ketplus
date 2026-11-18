#include "runner/remote/RemoteJobs.h"

#include "runner/sync/SnapshotPreparation.h"

#include <QLoggingCategory>

// A daemon start: the records a restart cut off end, the others resume.
namespace ketplus::daemon::runner::remote {

namespace {
Q_LOGGING_CATEGORY(lcRemoteRestore, "ketplus.runner.remote")

// Started, but the runner has not taken the spec: no runner ran any of it.
bool unsent(const jobs::RemoteJobRecord& record) {
    return !record.terminal() && record.state != jobs::state::needsAttention &&
           (record.placement == jobs::placement::submitting ||
            record.placement == jobs::placement::relayQueued ||
            record.placement == jobs::placement::assigned);
}
} // namespace

void RemoteJobs::restore() {
    if (!open_)
        return;
    const QList<jobs::RemoteJobRecord> loaded = store_.load();
    const QStringList unreadable = store_.unreadable();
    endUnsent(loaded);
    endUnreadable(unreadable);
    coordinator_.restore();
    live_.clear();
    QStringList liveTasks;
    for (const jobs::RemoteJobRecord& stored : loaded) {
        const auto current = coordinator_.record(stored.taskId);
        if (!current || current->terminal())
            continue;
        live_.insert(current->taskId);
        liveTasks.append(current->taskId);
        // The snapshot store of a kept record is mapped again; one without it cannot send.
        if (current->snapshot && ports_.stores) {
            snapshots_.put(
                current->taskId,
                TaskSnapshot{current->snapshot->repositoryId, current->snapshot->worktreeId,
                             sync::storePathFor(ports_.stores->storesRoot(),
                                                current->snapshot->repositoryId),
                             current->snapshot->snapshotId, current->snapshot->warnings});
        }
    }
    if (ports_.stores)
        ports_.stores->setLiveTasks(liveTasks);
    qCInfo(lcRemoteRestore).noquote()
        << QStringLiteral("remote jobs restored live=%1").arg(liveTasks.size());
}

// interrupted, daemon_restarted, cleanup not_required; the owner hears it through the task's
// completion, and the Relay is asked to cancel the job (a record without a job id submits
// again with the same key to learn it).
void RemoteJobs::endUnsent(const QList<jobs::RemoteJobRecord>& loaded) {
    for (const jobs::RemoteJobRecord& stored : loaded) {
        if (!unsent(stored))
            continue;
        jobs::RemoteJobRecord next = jobs::endUndispatched(stored, jobs::state::interrupted,
                                                           QStringLiteral("daemon_restarted"));
        if (!store_.put(next, stored.revision)) {
            qCWarning(lcRemoteRestore).noquote()
                << "remote job could not be ended after restart task=" + stored.taskId;
            continue;
        }
        specs_.forget(stored.taskId);
        cancels_.cancel(stored.taskId, stored.jobId, stored.runsOn);
    }
}

// interrupted, record_unreadable, cleanup unverified, without an acknowledgement; once only,
// since the row is written readable and terminal.
void RemoteJobs::endUnreadable(const QStringList& taskIds) {
    for (const QString& taskId : taskIds) {
        const std::optional<QString> jobId = store_.abandonUnreadable(taskId);
        if (!jobId) {
            qCWarning(lcRemoteRestore).noquote()
                << "remote job record could not be abandoned task=" + taskId;
            continue;
        }
        if (!jobId->isEmpty())
            cancels_.cancel(taskId, *jobId, {});
    }
}

} // namespace ketplus::daemon::runner::remote
