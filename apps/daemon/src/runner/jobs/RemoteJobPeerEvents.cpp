#include "runner/jobs/RemoteJobCoordinator.h"

// The inputs of a job's authenticated peer session, runner removal and the 20-minute window.
namespace ketplus::daemon::runner::jobs {

namespace {
const QString jobMismatch = QStringLiteral("job_mismatch");
const QString alreadyDispatched = QStringLiteral("already_dispatched");
const QString snapshotStop = QStringLiteral("snapshot_failed");
} // namespace

std::optional<RemoteJobRecord> RemoteJobCoordinator::recordOfJob(const QString& jobId) const {
    const auto task = jobs_.constFind(jobId);
    return jobId.isEmpty() || task == jobs_.cend() ? std::nullopt : record(*task);
}

RemoteJobRecord* RemoteJobCoordinator::byJob(const QString& jobId) {
    const auto found = jobs_.constFind(jobId);
    if (jobId.isEmpty() || found == jobs_.cend())
        return nullptr;
    auto record = records_.find(*found);
    return record == records_.end() || record->terminal() ? nullptr : &*record;
}

// The record whose spec went to `runnerId`, when that runner is still pinned.
RemoteJobRecord* RemoteJobCoordinator::fromRunner(const QString& jobId, const QString& runnerId) {
    RemoteJobRecord* record = byJob(jobId);
    if (!record || record->dispatchedTo != runnerId || !trust_.pinnedName(runnerId))
        return nullptr;
    return record;
}

JobDecision RemoteJobCoordinator::offer(const QString& jobId, const QString& runnerId,
                                        const int attempt) {
    RemoteJobRecord* found = byJob(jobId);
    if (!found)
        return {jobMismatch};
    if (!trust_.pinnedName(runnerId))
        return {QStringLiteral("not_paired")};
    if (found->dispatched()) {
        if (found->dispatchedTo == runnerId)
            return {}; // the same runner resumes; the spec is not sent again
        cancelOnRelay(found->taskId, alreadyDispatched);
        return {alreadyDispatched};
    }
    if (found->state == state::stopping ||
        (found->placement != placement::relayQueued && found->placement != placement::assigned))
        return {jobMismatch};
    RemoteJobRecord next = *found;
    next.state = state::starting;
    next.placement = placement::assigned;
    next.runner = RemoteRunner{runnerId, runnerName(runnerId), attempt};
    if (next != *found && !commit(next))
        return {QStringLiteral("storage_error")};
    return {};
}

JobDecision RemoteJobCoordinator::dispatch(const QString& jobId, const QString& runnerId) {
    RemoteJobRecord* found = byJob(jobId);
    if (!found)
        return {jobMismatch};
    if (found->dispatched())
        return {alreadyDispatched};
    if (!trust_.pinnedName(runnerId))
        return {QStringLiteral("not_paired")};
    if (found->state != state::starting || found->placement != placement::assigned ||
        !found->runner || found->runner->runnerId != runnerId)
        return {jobMismatch};
    RemoteJobRecord next = *found;
    const QDateTime now = clock_.now();
    next.dispatchedTo = runnerId;
    next.dispatchedAt = now;
    next.lastPeerContact = now;
    // Durable before the spec leaves: a failed write sends nothing.
    if (!commit(next))
        return {QStringLiteral("storage_error")};
    Live& live = live_[next.taskId];
    live.peerOpen = true;
    live.windowStart = now;
    return {};
}

void RemoteJobCoordinator::accepted(const QString& jobId, const QString& runnerId) {
    RemoteJobRecord* found = fromRunner(jobId, runnerId);
    if (!found || found->placement != placement::assigned)
        return;
    RemoteJobRecord next = *found;
    next.placement = placement::syncing;
    next.lastPeerContact = clock_.now();
    (void)commit(next); // a lost write is repeated by the next placement change
}

void RemoteJobCoordinator::started(const QString& jobId, const QString& runnerId) {
    RemoteJobRecord* found = fromRunner(jobId, runnerId);
    if (!found ||
        (found->placement != placement::assigned && found->placement != placement::syncing))
        return;
    RemoteJobRecord next = *found;
    next.placement = placement::running;
    if (next.state == state::starting)
        next.state = state::running;
    next.startedAt = clock_.now();
    next.lastPeerContact = next.startedAt;
    if (commit(next))
        armDeadline(records_.value(next.taskId));
}

JobDecision RemoteJobCoordinator::result(const QString& jobId, const QString& runnerId,
                                         const JobResult& value) {
    const auto task = jobs_.constFind(jobId);
    const auto current = task == jobs_.cend() ? std::nullopt : record(*task);
    if (!current || current->dispatchedTo != runnerId || !trust_.pinnedName(runnerId))
        return {jobMismatch};
    // A result recorded before changes nothing and is acknowledged again.
    RemoteJobRecord next = applyResult(*current, value);
    if (!current->terminal() && next.terminal() && current->stopReason == snapshotStop &&
        !current->startedAt && !value.exitCode && !value.signal) {
        next.state = state::failed; // the command never ran
        next.reason = snapshotStop;
    }
    if (next != *current && !commit(next))
        return {QStringLiteral("storage_error")};
    return {};
}

void RemoteJobCoordinator::rejected(const QString& jobId, const QString& runnerId,
                                    const QString& code) {
    RemoteJobRecord* found = byJob(jobId);
    if (!found || !trust_.pinnedName(runnerId) || found->placement != placement::assigned ||
        !found->runner || found->runner->runnerId != runnerId ||
        (found->dispatched() && found->dispatchedTo != runnerId))
        return;
    // The runner took no spec: its mark goes, and another runner may take the job.
    RemoteJobRecord next = *found;
    next.dispatchedTo.clear();
    next.dispatchedAt.reset();
    const auto reason = rejectionReason(code);
    if (next.state == state::stopping)
        next = endUndispatched(next, state::cancelled, QStringLiteral("user"));
    else if (reason)
        next = endUndispatched(next, state::failed, *reason);
    if (!commit(next))
        return;
    clock_.disarm(next.taskId, JobTimer::PeerAbsent);
    if (!next.terminal())
        refresh(next.taskId);
}

JobDecision RemoteJobCoordinator::snapshotFailed(const QString& jobId, const QString& runnerId,
                                                 const QString& code) {
    RemoteJobRecord* found = fromRunner(jobId, runnerId);
    if (!found || found->state != state::starting || found->placement != placement::syncing)
        return {QStringLiteral("not_applicable")};
    RemoteJobRecord next = *found;
    next.state = state::stopping;
    next.stopReason = snapshotStop;
    next.stopCode = code;
    next.cleanupStatus = cleanup::pending;
    if (!commit(next))
        return {QStringLiteral("storage_error")};
    return {};
}

void RemoteJobCoordinator::peerOpened(const QString& jobId, const QString& runnerId) {
    if (RemoteJobRecord* found = fromRunner(jobId, runnerId)) {
        live_[found->taskId].peerOpen = true;
        clock_.disarm(found->taskId, JobTimer::PeerAbsent);
    }
}

void RemoteJobCoordinator::peerLost(const QString& jobId, const QString& runnerId,
                                    const QDateTime& lastContact) {
    RemoteJobRecord* found = fromRunner(jobId, runnerId);
    if (!found)
        return;
    RemoteJobRecord next = *found;
    next.lastPeerContact = lastContact;
    (void)commit(next); // the window runs from memory either way
    armWindow(next.taskId, lastContact);
}

void RemoteJobCoordinator::armWindow(const QString& taskId, const QDateTime& from) {
    Live& live = live_[taskId];
    live.peerOpen = false;
    live.windowStart = from;
    const qint64 due = from.toMSecsSinceEpoch() + peerAbsentMs;
    clock_.arm(taskId, JobTimer::PeerAbsent,
               qMax<qint64>(0, due - clock_.now().toMSecsSinceEpoch()));
}

void RemoteJobCoordinator::windowElapsed(const QString& taskId) {
    const auto current = record(taskId);
    const auto live = live_.constFind(taskId);
    if (!current || current->terminal() || !current->dispatched() || live == live_.cend() ||
        live->peerOpen)
        return;
    const qint64 due = live->windowStart.toMSecsSinceEpoch() + peerAbsentMs;
    if (clock_.now().toMSecsSinceEpoch() < due) {
        armWindow(taskId, live->windowStart);
        return;
    }
    RemoteJobRecord next = *current;
    // The runner stayed away for the whole window: the daemon ends the job. Whether its
    // processes still run is beyond the daemon's reach and stays in the diagnostics.
    next.state = state::interrupted;
    next.reason = QStringLiteral("runner_unreachable");
    next.cleanupStatus = cleanup::unverified;
    if (!commit(next))
        clock_.arm(taskId, JobTimer::PeerAbsent, firstRetryMs);
    else
        clock_.disarm(taskId, JobTimer::Deadline);
}

JobDecision RemoteJobCoordinator::runnerRemoved(const QString& runnerId) {
    bool written = true;
    const QList<RemoteJobRecord> current = records_.values();
    for (const RemoteJobRecord& found : current) {
        if (found.terminal())
            continue;
        if (found.dispatchedTo == runnerId) {
            written = releaseRunner(found.taskId) && written;
        } else if (!found.dispatched() && found.runner && found.runner->runnerId == runnerId &&
                   !found.jobId.isEmpty()) {
            refresh(found.taskId);
        }
    }
    return written ? JobDecision{} : JobDecision{QStringLiteral("storage_error")};
}

// A runner that lost its pin can never report again: its records end at once. A record that
// cannot be written stays where it got to.
bool RemoteJobCoordinator::releaseRunner(const QString& taskId) {
    RemoteJobRecord next = records_.value(taskId);
    next.state = state::interrupted;
    next.reason = QStringLiteral("runner_removed");
    next.cleanupStatus = cleanup::unverified;
    if (!commit(next))
        return false;
    cancelOnRelay(taskId, QStringLiteral("runner_removed"));
    return true;
}

} // namespace ketplus::daemon::runner::jobs
