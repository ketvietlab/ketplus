#pragma once

#include "runner/jobs/RemoteJobPorts.h"

#include <QTimeZone>

#include <functional>
#include <memory>

// One contract for every RecordStore: the memory store and the store on the job registry run
// the same checks and must give the same results.
namespace remote_job_store_test {

using namespace ketplus::daemon::runner::jobs;

// A store under test, with the hooks the contract needs.
struct StoreUnderTest {
    RecordStore* store = nullptr;
    // Makes every later write fail, or succeed again.
    std::function<void(bool)> failWrites;
    // The same data seen by a fresh store, as after a daemon restart.
    std::function<RecordStore*()> reopen;
    // A task the store may hold a record for: runs_on {"os:linux"}, timeout 60 seconds.
    std::function<QString()> prepare;
};

inline QDateTime at(const int minute, const int msec = 0) {
    return QDateTime(QDate(2026, 11, 18), QTime(9, minute, 0, msec), QTimeZone::UTC);
}

// A record with every field set to a value other than its default.
inline RemoteJobRecord fullRecord(const QString& taskId) {
    RemoteJobRecord record;
    record.taskId = taskId;
    record.runsOn = {QStringLiteral("os:linux")};
    record.timeoutMs = 60'000;
    record.state = state::stopping;
    record.placement = placement::running;
    record.runner = RemoteRunner{QStringLiteral("runner-1"), QStringLiteral("build-1"), 2};
    record.reason = QStringLiteral("exit_code");
    record.cleanupStatus = cleanup::pending;
    record.jobId = QStringLiteral("job-") + taskId;
    record.stopReason = QStringLiteral("snapshot_failed");
    record.stopCode = QStringLiteral("disk_full");
    record.dispatchedTo = QStringLiteral("runner-1");
    record.dispatchedAt = at(1, 125);
    record.lastPeerContact = at(2, 250);
    record.startedAt = at(3, 375);
    record.exitCode = 3;
    record.signal = 9;
    return record;
}

inline RemoteJobRecord newRecord(const QString& taskId) {
    RemoteJobRecord record;
    record.taskId = taskId;
    record.runsOn = {QStringLiteral("os:linux")};
    record.timeoutMs = 60'000;
    return record;
}

} // namespace remote_job_store_test
