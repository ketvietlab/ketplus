#include "runner/jobs/RemoteJobCoordinator.h"

#include "runner/jobs/RemoteJobLabels.h"

namespace ketplus::daemon::runner::jobs {

namespace {
const QString user = QStringLiteral("user");
} // namespace

RemoteJobCoordinator::RemoteJobCoordinator(JobRelay& relay, RecordStore& store, JobClock& clock,
                                           const RunnerTrust& trust, JobPeers& peers)
    : relay_(relay), store_(store), clock_(clock), trust_(trust), peers_(peers) {}

std::optional<RemoteJobRecord> RemoteJobCoordinator::record(const QString& taskId) const {
    const auto found = records_.constFind(taskId);
    return found == records_.cend() ? std::nullopt : std::optional(*found);
}

int RemoteJobCoordinator::outstanding() const {
    int count = 0;
    for (const RemoteJobRecord& record : records_)
        count += record.terminal() ? 0 : 1;
    return count;
}

bool RemoteJobCoordinator::commit(const RemoteJobRecord& next) {
    const qint64 expected = records_.value(next.taskId).revision;
    if (!store_.put(next, expected))
        return false;
    RemoteJobRecord stored = next;
    stored.revision = expected + 1;
    records_.insert(next.taskId, stored);
    if (!next.jobId.isEmpty())
        jobs_.insert(next.jobId, next.taskId);
    if (stored.terminal()) {
        for (const JobTimer timer :
             {JobTimer::RelayRetry, JobTimer::PeerAbsent, JobTimer::Deadline})
            clock_.disarm(next.taskId, timer);
        live_.remove(next.taskId);
    }
    if (changed_)
        changed_(stored);
    return true;
}

QString RemoteJobCoordinator::runnerName(const QString& runnerId) const {
    return trust_.pinnedName(runnerId).value_or(runnerId);
}

JobDecision RemoteJobCoordinator::start(const QString& taskId, const QStringList& runsOn,
                                        const qint64 timeoutMs) {
    if (!isSubmitKey(taskId) || timeoutMs < 0)
        return {QStringLiteral("invalid_request")};
    if (!isValidRunsOn(runsOn))
        return {QStringLiteral("invalid_labels")};
    if (const auto existing = record(taskId))
        return existing->runsOn == runsOn && existing->timeoutMs == timeoutMs
                   ? JobDecision{}
                   : JobDecision{QStringLiteral("idempotency_conflict")};
    if (!relay_.available())
        return {QStringLiteral("runner_unavailable")};
    if (outstanding() >= maximumOutstanding)
        return {QStringLiteral("resource_exhausted")};
    RemoteJobRecord created;
    created.taskId = taskId;
    created.runsOn = runsOn;
    created.timeoutMs = timeoutMs;
    if (!commit(created))
        return {QStringLiteral("storage_error")};
    submit(taskId);
    return {};
}

void RemoteJobCoordinator::submit(const QString& taskId) {
    const RemoteJobRecord current = records_.value(taskId);
    relay_.submit(taskId, current.runsOn,
                  [this, taskId](const JobOutcome& outcome) { submitted(taskId, outcome); });
}

void RemoteJobCoordinator::submitted(const QString& taskId, const JobOutcome& outcome) {
    const auto current = record(taskId);
    if (!current || current->terminal() || !current->jobId.isEmpty())
        return;
    const bool stopping = current->state == state::stopping;
    if (!outcome.ok()) {
        const auto reason = submitRefusalReason(outcome.failure.code);
        if (!reason) {
            retryLater(taskId);
            return;
        }
        const RemoteJobRecord ended = stopping ? endUndispatched(*current, state::cancelled, user)
                                               : endUndispatched(*current, state::failed, *reason);
        if (!commit(ended))
            retryLater(taskId);
        return;
    }
    live_[taskId].retryMs = firstRetryMs;
    RemoteJobRecord next = *current;
    next.jobId = outcome.value->jobId;
    const auto& runnerId = outcome.value->runnerId;
    if (const auto mapped =
            applyRelayJob(next, *outcome.value, runnerId ? runnerName(*runnerId) : ""))
        next = *mapped;
    if (next.placement == placement::submitting && !next.terminal())
        next.placement = placement::relayQueued;
    if (!commit(next)) {
        retryLater(taskId); // the same key answers the same job
        return;
    }
    if (stopping && !next.terminal())
        cancelOnRelay(taskId, user);
}

void RemoteJobCoordinator::retryLater(const QString& taskId) {
    Live& live = live_[taskId];
    const qint64 delay = live.retryMs;
    live.retryMs = qMin(delay * 2, longestRetryMs);
    clock_.arm(taskId, JobTimer::RelayRetry, delay);
}

void RemoteJobCoordinator::jobChanged(const QString& jobId) {
    const QString taskId = jobs_.value(jobId);
    const auto current = record(taskId);
    if (current && !current->terminal() && !current->dispatched())
        refresh(taskId);
}

void RemoteJobCoordinator::refresh(const QString& taskId) {
    const RemoteJobRecord current = records_.value(taskId);
    relay_.get(current.jobId, [this, taskId](const JobOutcome& outcome) {
        const auto now = record(taskId);
        if (!now || now->terminal() || now->dispatched())
            return;
        if (outcome.ok()) {
            live_[taskId].retryMs = firstRetryMs;
            applyRelay(taskId, *outcome.value);
        } else if (outcome.failure.code == QStringLiteral("job_not_found")) {
            const bool stopping = now->state == state::stopping;
            if (!commit(stopping ? endUndispatched(*now, state::cancelled, user)
                                 : endUndispatched(*now, state::failed,
                                                   QStringLiteral("runner_unavailable"))))
                retryLater(taskId);
        } else {
            retryLater(taskId);
        }
    });
}

void RemoteJobCoordinator::applyRelay(const QString& taskId, const RelayJob& job) {
    const auto current = record(taskId);
    if (!current)
        return;
    const auto next = applyRelayJob(*current, job, job.runnerId ? runnerName(*job.runnerId) : "");
    if (next && !commit(*next))
        retryLater(taskId);
}

JobDecision RemoteJobCoordinator::stop(const QString& taskId) {
    const auto current = record(taskId);
    if (!current)
        return {QStringLiteral("not_found")};
    if (current->terminal() || current->state == state::stopping ||
        current->state == state::needsAttention)
        return {};
    RemoteJobRecord next = *current;
    next.state = state::stopping;
    next.stopReason = user;
    next.cleanupStatus = current->dispatched() ? cleanup::pending : cleanup::notRequired;
    if (!commit(next))
        return {QStringLiteral("storage_error")};
    if (const QString holder =
            next.dispatched() ? next.dispatchedTo : (next.runner ? next.runner->runnerId : "");
        !holder.isEmpty())
        peers_.cancel(taskId, holder);
    if (next.jobId.isEmpty())
        submit(taskId); // the answer names the job to cancel
    else
        cancelOnRelay(taskId, user);
    return {};
}

void RemoteJobCoordinator::cancelOnRelay(const QString& taskId, const QString& reason) {
    const RemoteJobRecord current = records_.value(taskId);
    if (current.jobId.isEmpty())
        return;
    if (reason != user) {
        relay_.cancel(current.jobId, reason, [](const JobOutcome&) {});
        return;
    }
    relay_.cancel(current.jobId, reason,
                  [this, taskId](const JobOutcome& outcome) { cancelAnswered(taskId, outcome); });
}

void RemoteJobCoordinator::cancelAnswered(const QString& taskId, const JobOutcome& outcome) {
    const auto current = record(taskId);
    if (!current || current->terminal() || current->state != state::stopping ||
        current->dispatched())
        return; // a dispatched job ends with the runner's result or the 20-minute window
    if (!outcome.ok() && outcome.failure.code != QStringLiteral("job_not_found")) {
        retryLater(taskId);
        return;
    }
    if (!commit(endUndispatched(*current, state::cancelled, user)))
        retryLater(taskId);
}

JobDecision RemoteJobCoordinator::resolveCleanup(const QString& taskId) {
    const auto current = record(taskId);
    if (!current)
        return {QStringLiteral("not_found")};
    if (current->state != state::needsAttention)
        return {QStringLiteral("invalid_state")};
    RemoteJobRecord next = *current;
    next.state = state::interrupted;
    next.reason = QStringLiteral("cleanup_acknowledged");
    if (!commit(next))
        return {QStringLiteral("storage_error")};
    return {};
}

void RemoteJobCoordinator::timerFired(const QString& taskId, const JobTimer timer) {
    const auto current = record(taskId);
    if (!current || current->terminal())
        return;
    switch (timer) {
    case JobTimer::RelayRetry:
        if (current->jobId.isEmpty())
            submit(taskId);
        else if (current->state == state::stopping && current->stopReason == user &&
                 !current->dispatched())
            cancelOnRelay(taskId, user);
        else if (!current->dispatched())
            refresh(taskId);
        return;
    case JobTimer::PeerAbsent:
        windowElapsed(taskId);
        return;
    case JobTimer::Deadline:
        if (current->state != state::running)
            return;
        RemoteJobRecord next = *current;
        next.state = state::stopping;
        next.stopReason = QStringLiteral("deadline");
        next.cleanupStatus = cleanup::pending;
        if (!commit(next))
            clock_.arm(taskId, JobTimer::Deadline, firstRetryMs);
        return;
    }
}

void RemoteJobCoordinator::armDeadline(const RemoteJobRecord& record) {
    if (!record.startedAt || record.timeoutMs <= 0 || record.state != state::running)
        return;
    const qint64 due = record.startedAt->toMSecsSinceEpoch() + record.timeoutMs;
    clock_.arm(record.taskId, JobTimer::Deadline,
               qMax<qint64>(0, due - clock_.now().toMSecsSinceEpoch()));
}

void RemoteJobCoordinator::restore() {
    records_.clear();
    jobs_.clear();
    live_.clear();
    for (const RemoteJobRecord& stored : store_.load()) {
        records_.insert(stored.taskId, stored);
        if (!stored.jobId.isEmpty())
            jobs_.insert(stored.jobId, stored.taskId);
    }
    const QDateTime now = clock_.now();
    // Answers may come at once and change the records, so this walks a copy.
    const QList<RemoteJobRecord> loaded = records_.values();
    for (const RemoteJobRecord& stored : loaded) {
        if (stored.terminal())
            continue;
        const bool userStop = stored.state == state::stopping && stored.stopReason == user;
        if (stored.jobId.isEmpty()) {
            submit(stored.taskId);
        } else if (stored.dispatched()) {
            // The runner offers again; the spec is never sent twice.
            if (stored.state != state::needsAttention)
                armWindow(stored.taskId, now);
            if (userStop)
                cancelOnRelay(stored.taskId, user);
            armDeadline(stored);
        } else if (userStop) {
            cancelOnRelay(stored.taskId, user);
        }
    }
    reconcile();
}

void RemoteJobCoordinator::reconcile() {
    const QList<RemoteJobRecord> current = records_.values();
    for (const RemoteJobRecord& found : current) {
        if (found.terminal() || found.dispatched() || found.jobId.isEmpty() ||
            found.state == state::stopping)
            continue;
        if (found.placement == placement::relayQueued || found.placement == placement::assigned)
            refresh(found.taskId);
    }
}

} // namespace ketplus::daemon::runner::jobs
