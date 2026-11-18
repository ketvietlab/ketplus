#include "session/JobSession.h"

#include <QRegularExpression>

namespace ketplus::runner::session {

namespace {

const QString cancelled = QStringLiteral("cancelled");
const QString interrupted = QStringLiteral("interrupted");

// The Relay's cancel reason when it fits a result reason, otherwise "cancelled".
QString relayCancelReason(const QString& reason) {
    static const QRegularExpression code(QStringLiteral("^[a-z0-9_]{1,64}$"));
    return code.match(reason).hasMatch() ? reason : cancelled;
}

} // namespace

// A desktop or Relay cancel. Repeats change nothing.
void JobSession::cancel(const QString& reason) {
    switch (m_phase) {
    case Phase::Idle:
    case Phase::Stopping:
    case Phase::Delivering:
        return;
    case Phase::Assigned: // no spec yet: the job is dropped
        finishLease(cancelled, reason);
        endJob();
        return;
    case Phase::Starting:
        if (!m_job->pendingCancel)
            m_job->pendingCancel = reason;
        return;
    case Phase::Syncing:
        m_ports.snapshots.cancel();
        endWithoutCommand(cancelled, reason);
        return;
    case Phase::Running:
        stop(cancelled, reason);
        return;
    }
}

// Kills the command; the result follows its cleanup. The first reason to stop wins.
void JobSession::stop(const QString& state, const QString& reason) {
    if (m_phase != Phase::Running)
        return;
    m_job->stopping = {state, reason};
    m_phase = Phase::Stopping;
    m_ports.clock.disarm(SessionTimer::JobTimeout);
    m_ports.clock.disarm(SessionTimer::PeerAbsent);
    m_ports.command.kill();
}

bool JobSession::withdraw() {
    if (m_phase != Phase::Assigned)
        return false;
    if (m_link != Link::None)
        closeChannel(QStringLiteral("draining"));
    endJob();
    return true;
}

void JobSession::leaseLost(const QString& code, const QString& reason) {
    if (m_phase == Phase::Idle)
        return;
    if (code == QStringLiteral("unsupported")) {
        // Before `job.start` the spec is refused; after it the job runs to its end.
        m_job->relayUnsupported = true;
        return;
    }
    if (m_phase == Phase::Delivering) {
        // The result is reported to the Relay once and dropped.
        finishResult();
        endJob();
        return;
    }
    if (code == QStringLiteral("job_cancelled")) {
        cancel(relayCancelReason(reason));
        return;
    }
    const QString lost =
        code == QStringLiteral("entitlement_required") ? code : QStringLiteral("lease_lost");
    switch (m_phase) {
    case Phase::Idle:
    case Phase::Delivering:
        return;
    case Phase::Assigned: // nothing was taken: drop the job and its connection
        if (m_link != Link::None)
            closeChannel(QStringLiteral("lease_lost"));
        endJob();
        return;
    case Phase::Starting: {
        const JobAssignment job = m_job->assignment;
        reject(job.jobId, job.attempt, protocol::ErrorCode::LeaseLost);
        endJob();
        return;
    }
    case Phase::Syncing:
        m_job->leaseGone = true;
        m_ports.snapshots.cancel();
        endWithoutCommand(interrupted, lost);
        return;
    case Phase::Running:
    case Phase::Stopping:
        m_job->leaseGone = true;
        stop(interrupted, lost);
        return;
    }
}

void JobSession::timerFired(const SessionTimer timer) {
    if (m_phase == Phase::Idle)
        return;
    switch (timer) {
    case SessionTimer::PeerOpen:
        if (m_phase == Phase::Assigned) { // no `welcome`, or no spec after it
            if (m_link != Link::None)
                m_ports.channel.close(QStringLiteral("peer_unreachable"));
            m_link = Link::None;
            m_ports.lease.release(QStringLiteral("peer_unreachable"));
            endJob();
        }
        return;
    case SessionTimer::Reconnect:
        if (m_link == Link::None) {
            m_ports.channel.requestConnection();
            m_ports.clock.arm(SessionTimer::Reconnect, reconnectMs);
        }
        return;
    case SessionTimer::PeerAbsent:
        if (m_link == Link::Open)
            return;
        if (m_phase == Phase::Syncing) {
            m_ports.snapshots.cancel();
            endWithoutCommand(interrupted, QStringLiteral("requester_lost"));
        } else {
            stop(interrupted, QStringLiteral("requester_lost"));
        }
        return;
    case SessionTimer::JobTimeout:
        stop(QStringLiteral("timed_out"), QString());
        return;
    case SessionTimer::ResultRetention:
        if (m_phase == Phase::Delivering) {
            // Never acknowledged: the result is dropped and the lease ends with it.
            finishResult();
            endJob();
        }
        return;
    }
}

} // namespace ketplus::runner::session
