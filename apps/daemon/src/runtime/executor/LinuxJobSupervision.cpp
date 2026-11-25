#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobLease.h"
#include "runtime/executor/NativeJobSupervisor.h"

#include <QCoreApplication>
#include <csignal>

namespace ketplus::daemon::executor {
bool supervisorStopRequested();

void NativeJobSupervisor::linuxTick() {
    refreshKnown();
    const auto boundary = metadata_.value("linux_boundary").toObject();
    const bool stopped = process_.state() == QProcess::NotRunning && linuxBoundaryEmpty(boundary);
    if (finalized_) {
        if (stopped && clock_.elapsed() - lastPulse_ >= NativeJobLeaseMs)
            QCoreApplication::quit();
        return;
    }
    if (leaseExpired() || supervisorStopRequested())
        requestStop("interrupted");
    if (deadline_ > 0 && clock_.elapsed() >= deadline_)
        requestStop("timed_out");
    if (stopping_ && clock_.elapsed() >= killAt_)
        signalKnown(SIGKILL); // cgroup.kill includes concurrent forks and setsid descendants.
    if (rootExited_ && !linuxBoundaryEmpty(boundary))
        signalKnown(SIGKILL); // Preserve the root result while reclaiming its descendants.
    if (stopping_ && clock_.elapsed() >= finishCleanupAt_ && !stopped) {
        metadata_.insert("cleanup_pending", true);
        metadata_.insert("cleanup_status", "pending");
        metadata_.insert("cleanup_issue", "linux_cleanup_pending");
        persist();
        send("update", metadata_);
    }
    if (stopped && (rootExited_ || stopping_))
        finish();
}
} // namespace ketplus::daemon::executor
