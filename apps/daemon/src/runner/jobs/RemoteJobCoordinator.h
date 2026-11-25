#pragma once

#include "runner/jobs/RemoteJobPorts.h"
#include "runner/jobs/RemoteJobRecord.h"
#include "runner/jobs/RemoteJobRelay.h"

#include <QHash>
#include <QSet>
#include <QString>

#include <functional>

// The desktop's remote job records. Inputs come from the start and Stop requests, the Relay's
// answers and job.changed hints, the authenticated peer session of each job, timers, runner
// removal; effects go through the ports.
namespace ketplus::daemon::runner::jobs {

inline constexpr int maximumOutstanding = 64;
inline constexpr qint64 firstRetryMs = 1000;
inline constexpr qint64 longestRetryMs = 30000;
inline constexpr qint64 peerAbsentMs = 20 * 60 * 1000;

// An answer to the caller: an empty code means it was done.
struct JobDecision {
    QString code;
    [[nodiscard]] bool ok() const { return code.isEmpty(); }
};

class RemoteJobCoordinator final {
  public:
    using Changed = std::function<void(const RemoteJobRecord&)>;

    RemoteJobCoordinator(JobRelay& relay, RecordStore& store, JobClock& clock,
                         const RunnerTrust& trust, JobPeers& peers);

    void setChanged(Changed changed) { changed_ = std::move(changed); }
    [[nodiscard]] std::optional<RemoteJobRecord> record(const QString& taskId) const;
    [[nodiscard]] std::optional<RemoteJobRecord> recordOfJob(const QString& jobId) const;
    [[nodiscard]] int outstanding() const;

    // Start and Stop.
    JobDecision start(const QString& taskId, const QStringList& runsOn, qint64 timeoutMs);
    // The same, with the snapshot frozen for the job in its first record.
    JobDecision start(const QString& taskId, const QStringList& runsOn, qint64 timeoutMs,
                      const RemoteSnapshot& snapshot);
    JobDecision stop(const QString& taskId);

    // The Relay.
    void jobChanged(const QString& jobId);

    // The peer session of a job. `offer` answers whether the session may go on with this runner
    // (empty code) or must end it with the code; `dispatch` whether the spec may be sent now.
    JobDecision offer(const QString& jobId, const QString& runnerId, int attempt);
    JobDecision dispatch(const QString& jobId, const QString& runnerId);
    void accepted(const QString& jobId, const QString& runnerId);
    void started(const QString& jobId, const QString& runnerId);
    // Ok once the result is durable (also for a result already recorded), so the session may
    // acknowledge it; storage_error leaves the record unchanged for the runner to send again.
    JobDecision result(const QString& jobId, const QString& runnerId, const JobResult& result);
    void rejected(const QString& jobId, const QString& runnerId, const QString& code);
    // The desktop could not send the snapshot of an accepted job that has not started: the
    // record stops and keeps `code`, and the runner's result ends it. not_applicable in any other
    // state, storage_error when it could not be written; neither changes the record.
    JobDecision snapshotFailed(const QString& jobId, const QString& runnerId, const QString& code);
    void peerOpened(const QString& jobId, const QString& runnerId);
    void peerLost(const QString& jobId, const QString& runnerId, const QDateTime& lastContact);

    // The user removed or unpaired a runner.
    // storage_error when a record of the runner could not be written.
    JobDecision runnerRemoved(const QString& runnerId);
    // After a daemon restart: loads the stored records and resumes each one.
    void restore();
    // Asks the Relay again about every record whose spec was never dispatched, as after a
    // restart; for a Relay connection that came back, when hints may have been missed.
    void reconcile();
    void timerFired(const QString& taskId, JobTimer timer);

  private:
    JobDecision begin(const QString& taskId, const QStringList& runsOn, qint64 timeoutMs,
                      const std::optional<RemoteSnapshot>& snapshot);
    struct Live {
        qint64 retryMs = firstRetryMs;
        bool peerOpen = false;
        QDateTime windowStart; // the 20-minute window counts from here
    };

    [[nodiscard]] bool commit(const RemoteJobRecord& next);
    [[nodiscard]] RemoteJobRecord* byJob(const QString& jobId);
    [[nodiscard]] RemoteJobRecord* fromRunner(const QString& jobId, const QString& runnerId);
    void submit(const QString& taskId);
    void submitted(const QString& taskId, const JobOutcome& outcome);
    void refresh(const QString& taskId);
    void applyRelay(const QString& taskId, const RelayJob& job);
    void cancelOnRelay(const QString& taskId, const QString& reason);
    void cancelAnswered(const QString& taskId, const JobOutcome& outcome);
    void retryLater(const QString& taskId);
    void armWindow(const QString& taskId, const QDateTime& from);
    void armDeadline(const RemoteJobRecord& record);
    void windowElapsed(const QString& taskId);
    [[nodiscard]] bool releaseRunner(const QString& taskId);
    [[nodiscard]] QString runnerName(const QString& runnerId) const;

    JobRelay& relay_;
    RecordStore& store_;
    JobClock& clock_;
    const RunnerTrust& trust_;
    JobPeers& peers_;
    Changed changed_;
    QHash<QString, RemoteJobRecord> records_;
    QHash<QString, QString> jobs_; // jobId -> taskId
    QHash<QString, Live> live_;
};

} // namespace ketplus::daemon::runner::jobs
