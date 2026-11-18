#include "runner/remote/CoordinatorRecords.h"

#include <limits>

namespace ketplus::daemon::runner::remote {

namespace {
peer::RecordAnswer answer(const jobs::JobDecision& decision) { return {decision.code}; }
} // namespace

peer::RecordView recordView(const jobs::RemoteJobRecord& record) {
    peer::RecordView view;
    view.dispatchedTo = record.dispatchedTo;
    view.runsOn = record.runsOn;
    view.accepted = record.placement == jobs::placement::syncing ||
                    record.placement == jobs::placement::running;
    view.started = record.placement == jobs::placement::running;
    view.stopReason = record.stopReason;
    view.terminal = record.terminal();
    return view;
}

std::optional<peer::RecordView> CoordinatorRecords::view(const QString& jobId) const {
    const auto found = coordinator_.recordOfJob(jobId);
    if (!found)
        return std::nullopt;
    return recordView(*found);
}

peer::RecordAnswer CoordinatorRecords::offer(const QString& jobId, const QString& runnerId,
                                             const qint64 attempt) {
    // The attempt counts assignments of one job; a value beyond int is not a real attempt.
    if (attempt < 1 || attempt > std::numeric_limits<int>::max())
        return {QStringLiteral("job_mismatch")};
    return answer(coordinator_.offer(jobId, runnerId, static_cast<int>(attempt)));
}

peer::RecordAnswer CoordinatorRecords::dispatch(const QString& jobId, const QString& runnerId) {
    return answer(coordinator_.dispatch(jobId, runnerId));
}

void CoordinatorRecords::accepted(const QString& jobId, const QString& runnerId) {
    coordinator_.accepted(jobId, runnerId);
}

void CoordinatorRecords::started(const QString& jobId, const QString& runnerId) {
    coordinator_.started(jobId, runnerId);
}

void CoordinatorRecords::rejected(const QString& jobId, const QString& runnerId,
                                  const QString& code) {
    coordinator_.rejected(jobId, runnerId, code);
}

peer::RecordAnswer CoordinatorRecords::snapshotFailed(const QString& jobId, const QString& runnerId,
                                                      const QString& code) {
    return answer(coordinator_.snapshotFailed(jobId, runnerId, code));
}

peer::RecordAnswer CoordinatorRecords::result(const QString& jobId, const QString& runnerId,
                                              const peer::protocol::JobResult& result) {
    jobs::JobResult recorded;
    recorded.state = result.state;
    recorded.exitCode = result.exitCode;
    recorded.signal = result.signal;
    recorded.reason = result.reason;
    recorded.cleanupStatus = result.cleanupStatus;
    return answer(coordinator_.result(jobId, runnerId, recorded));
}

void CoordinatorRecords::peerOpened(const QString& jobId, const QString& runnerId) {
    coordinator_.peerOpened(jobId, runnerId);
}

void CoordinatorRecords::peerLost(const QString& jobId, const QString& runnerId,
                                  const QDateTime& lastContact) {
    coordinator_.peerLost(jobId, runnerId, lastContact);
}

QString CoordinatorRecords::jobOf(const QString& taskId) const {
    const auto found = coordinator_.record(taskId);
    return found ? found->jobId : QString();
}

} // namespace ketplus::daemon::runner::remote
