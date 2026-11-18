#include "session/JobSession.h"

namespace ketplus::runner::session {

using namespace protocol;

namespace {

QString utc(const QDateTime& time) {
    return time.toUTC()
        .toString(Qt::ISODateWithMs)
        .replace(QStringLiteral("+00:00"), QStringLiteral("Z"));
}

// The `reason` of `job.finish` for a result.
QString finishReason(const JobResult& result) {
    if (result.state == QStringLiteral("exited")) {
        if (result.signal)
            return QStringLiteral("signaled");
        return result.exitCode == 0 ? QStringLiteral("exited") : QStringLiteral("exited_nonzero");
    }
    if (result.state == QStringLiteral("timed_out") || result.reason.isEmpty())
        return result.state;
    return result.reason; // "cancelled", "interrupted" and "failed"
}

} // namespace

QString finishOutcome(const JobResult& result) {
    if (result.state == QStringLiteral("exited"))
        return !result.signal && result.exitCode == 0 ? QStringLiteral("succeeded")
                                                      : QStringLiteral("failed");
    if (result.state == QStringLiteral("cancelled") ||
        result.state == QStringLiteral("interrupted"))
        return result.state;
    return QStringLiteral("failed"); // "timed_out" and "failed"
}

void JobSession::commandFinished(const CommandExit& exit) {
    if (m_phase != Phase::Running && m_phase != Phase::Stopping)
        return;
    m_ports.clock.disarm(SessionTimer::JobTimeout);
    JobResult result;
    result.jobId = m_job->assignment.jobId;
    result.attempt = m_job->assignment.attempt;
    if (m_job->stopping) {
        result.state = m_job->stopping->first;
        result.reason = m_job->stopping->second;
    } else {
        result.state = exit.state;
        result.reason = exit.reason;
    }
    result.signal = exit.signal;
    result.exitCode = exit.signal ? std::nullopt : exit.exitCode; // never both
    result.cleanupStatus = exit.cleanupStatus;
    result.outputBytes = m_output.end();
    result.startedAt = m_job->startedAt;
    result.endedAt = utc(m_ports.clock.now());
    result.warnings = m_job->warnings;
    deliver(std::move(result));
}

// A result for a job whose command never ran: nothing to clean up.
void JobSession::endWithoutCommand(const QString& state, const QString& reason) {
    JobResult result;
    result.jobId = m_job->assignment.jobId;
    result.attempt = m_job->assignment.attempt;
    result.state = state;
    result.reason = reason;
    result.cleanupStatus = QStringLiteral("verified");
    result.endedAt = utc(m_ports.clock.now());
    result.warnings = m_job->warnings;
    deliver(std::move(result));
}

// Holds the result and sends it until the desktop acknowledges it, for at most 60 minutes. When
// the Relay no longer holds the lease, the result is sent once if it can be and the job ends.
void JobSession::deliver(JobResult result) {
    m_job->result = std::move(result);
    m_phase = Phase::Delivering;
    m_ports.clock.disarm(SessionTimer::JobTimeout);
    m_ports.clock.disarm(SessionTimer::PeerAbsent);
    m_ports.clock.arm(SessionTimer::ResultRetention, resultRetentionMs);
    m_resultPending = true;
    flush();
    if (m_job->leaseGone) {
        finishResult();
        endJob();
    }
}

void JobSession::sendResult() { send(*m_job->result); }

void JobSession::onResultAck() {
    if (m_phase != Phase::Delivering)
        return;
    finishResult();
    endJob();
}

void JobSession::finishResult() {
    finishLease(finishOutcome(*m_job->result), finishReason(*m_job->result));
}

void JobSession::finishLease(const QString& outcome, const QString& reason) {
    if (m_job->finished)
        return;
    m_job->finished = true;
    m_ports.lease.finish(outcome, reason);
}

void JobSession::endJob() {
    for (const SessionTimer timer :
         {SessionTimer::PeerOpen, SessionTimer::Reconnect, SessionTimer::PeerAbsent,
          SessionTimer::JobTimeout, SessionTimer::ResultRetention})
        m_ports.clock.disarm(timer);
    m_job.reset();
    m_phase = Phase::Idle;
    m_streaming = false;
    m_output.clear();
    m_sentUpTo = 0;
    m_resultPending = false;
    ++m_generation;
}

} // namespace ketplus::runner::session
