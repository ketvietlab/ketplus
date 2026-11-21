#include "protocol/FrameLimits.h"
#include "session/JobSession.h"

#include <algorithm>

namespace ketplus::runner::session {

using namespace protocol;

void JobSession::commandOutput(const QByteArray& bytes) {
    if (m_phase != Phase::Running && m_phase != Phase::Stopping)
        return;
    m_output.append(bytes);
    sendOutput();
}

// Output the command wrote that was lost before it reached the session: it counts as written and
// is reported as a gap.
void JobSession::commandOutputLost(const qint64 bytes) {
    if ((m_phase != Phase::Running && m_phase != Phase::Stopping) || bytes <= 0)
        return;
    m_output.skip(bytes);
    sendOutput();
}

// Pages of at most 32 KiB from the first byte not yet sent on this connection, while `events`
// takes them. When that byte is no longer held, one gap reports the bytes lost before the pages go
// on from the first byte held. Sent bytes that leave the buffer make no gap; bytes that never
// reached it make one.
void JobSession::sendOutput() {
    if (m_link != Link::Open || !m_streaming)
        return;
    const QString& jobId = m_job->assignment.jobId;
    while (m_sentUpTo < m_output.end() && m_ports.channel.eventsWritable()) {
        const qint64 held = m_output.heldFrom(m_sentUpTo);
        if (m_sentUpTo < held) {
            if (!send(JobOutputGap{jobId, m_sentUpTo, held}))
                return;
            m_sentUpTo = held;
            continue;
        }
        const QByteArray page = m_output.read(m_sentUpTo, limits::maxOutputBytes);
        if (page.isEmpty() || !send(JobOutput{jobId, m_sentUpTo, page}))
            return;
        m_sentUpTo += page.size();
    }
}

void JobSession::eventsWritable() {
    if (m_phase == Phase::Running || m_phase == Phase::Stopping || m_phase == Phase::Delivering)
        flush();
}

void JobSession::flush() {
    sendOutput();
    if (m_phase != Phase::Delivering || !m_resultPending || m_link != Link::Open || !m_streaming ||
        m_sentUpTo < m_output.end())
        return;
    m_resultPending = false;
    sendResult();
}

// The desktop wants the output again from `fromOffset`.
void JobSession::resumeOutput(const qint64 fromOffset) {
    m_streaming = true;
    m_sentUpTo = std::min(fromOffset, m_output.end());
    flush();
}

// `job.resume` on a new connection: where the job is, then the output the desktop lacks, then
// the result when there is one.
void JobSession::onResume(const JobResume& resume) {
    if (!isCurrent(resume.jobId) || !m_job->spec || m_phase == Phase::Starting)
        return;
    const JobAssignment& job = m_job->assignment;
    if (!m_job->launched) {
        send(JobAccepted{job.jobId, job.attempt});
        m_streaming = true;
        m_resultPending = m_job->result.has_value();
        flush();
        return;
    }
    send(JobStarted{job.jobId, m_job->startedAt});
    m_output.acknowledge(resume.outputOffset); // already in the desktop's spool
    m_resultPending = m_job->result.has_value();
    resumeOutput(resume.outputOffset);
}

} // namespace ketplus::runner::session
