#include "session/JobSession.h"

namespace ketplus::runner::session {

using namespace protocol;

void JobSession::onSpec(const JobSpec& spec) {
    const bool leased =
        m_job && spec.jobId == m_job->assignment.jobId && spec.attempt == m_job->assignment.attempt;
    if (!leased) {
        // Another job: refused while this one has a spec, otherwise it is not the leased job.
        const bool busy = m_job && m_job->spec.has_value();
        reject(spec.jobId, spec.attempt, busy ? ErrorCode::RunnerBusy : ErrorCode::JobMismatch);
        return;
    }
    if (m_job->spec) {
        answerCurrentState(); // the same spec again: never a second launch
        return;
    }
    if (m_ports.accepted.contains(spec.jobId, spec.attempt)) {
        // Taken before, in a state this session no longer has: never run it again.
        reject(spec.jobId, spec.attempt, ErrorCode::JobMismatch);
        return;
    }
    if (m_job->relayUnsupported) {
        refuseSpec(ErrorCode::RunnerUnavailable);
        return;
    }
    if (!m_ports.command.admit()) {
        refuseSpec(ErrorCode::ContainmentUnavailable);
        return;
    }
    m_ports.clock.disarm(SessionTimer::PeerOpen);
    if (!m_ports.accepted.add(spec.jobId, spec.attempt)) {
        refuseSpec(ErrorCode::RunnerUnavailable); // not recorded: never run
        return;
    }
    m_job->spec = spec;
    m_phase = Phase::Starting;
    m_ports.lease.start(); // `job.accepted` only once the Relay has started the job
}

void JobSession::onInvalidSpec(const QString& jobId, const qint64 attempt) {
    const bool leased =
        m_job && jobId == m_job->assignment.jobId && attempt == m_job->assignment.attempt;
    if (!leased) {
        const bool busy = m_job && m_job->spec.has_value();
        reject(jobId, attempt, busy ? ErrorCode::RunnerBusy : ErrorCode::JobMismatch);
        return;
    }
    if (m_job->spec) {
        answerCurrentState();
        return;
    }
    refuseSpec(ErrorCode::SpecInvalid);
}

// The leased job's spec is refused before `job.start`: nothing ran, and the lease ends.
void JobSession::refuseSpec(const ErrorCode code) {
    const JobAssignment job = m_job->assignment;
    reject(job.jobId, job.attempt, code);
    finishLease(QStringLiteral("rejected"), QString(toString(code)));
    endJob();
}

void JobSession::leaseStarted(const QString& startedAt) {
    if (m_phase != Phase::Starting)
        return;
    m_job->startedAt = startedAt;
    send(JobAccepted{m_job->assignment.jobId, m_job->assignment.attempt});
    if (m_job->pendingCancel) {
        endWithoutCommand(QStringLiteral("cancelled"), *m_job->pendingCancel);
        return;
    }
    beginSnapshot();
}

void JobSession::leaseStartRefused(const QString& code) {
    if (m_phase != Phase::Starting)
        return;
    const bool withdrawn = code == QStringLiteral("unsupported");
    const JobAssignment job = m_job->assignment;
    reject(job.jobId, job.attempt, withdrawn ? ErrorCode::RunnerUnavailable : ErrorCode::LeaseLost);
    if (withdrawn)
        finishLease(QStringLiteral("rejected"), QString(toString(ErrorCode::RunnerUnavailable)));
    endJob();
}

void JobSession::beginSnapshot() {
    m_phase = Phase::Syncing;
    if (m_link == Link::None)
        m_ports.clock.arm(SessionTimer::PeerAbsent, peerAbsentMs);
    const quint64 generation = m_generation;
    m_ports.snapshots.begin(*m_job->spec, m_job->assignment.machineId,
                            [this, generation](const Frame& frame) {
                                if (generation == m_generation && m_phase == Phase::Syncing)
                                    send(frame);
                            });
}

void JobSession::snapshotReady(const QString& workspace, const QStringList& warnings) {
    if (m_phase != Phase::Syncing)
        return;
    const JobSpec& spec = *m_job->spec;
    m_job->warnings = warnings;
    send(SnapshotReady{spec.snapshotId});
    m_job->launched = true;
    m_phase = Phase::Running;
    m_ports.command.launch(launchRequest(workspace));
    if (m_phase != Phase::Running)
        return; // the runner reported the end of the command from inside launch()
    send(JobStarted{spec.jobId, m_job->startedAt});
    m_ports.clock.arm(SessionTimer::JobTimeout, spec.timeoutMs);
    m_streaming = m_streaming || m_link == Link::Open;
}

void JobSession::snapshotFailed(const QString& code) {
    if (m_phase != Phase::Syncing)
        return;
    send(SnapshotError{m_job->spec->snapshotId, code});
    endWithoutCommand(QStringLiteral("failed"), code);
}

LaunchRequest JobSession::launchRequest(const QString& workspace) const {
    const JobSpec& spec = *m_job->spec;
    LaunchRequest request;
    request.invocation = spec.invocation;
    request.workspace = workspace;
    request.cwd = spec.cwd;
    request.environment = m_runner.baseEnvironment;
    for (auto it = spec.env.cbegin(); it != spec.env.cend(); ++it) {
        if (it.value())
            request.environment.insert(it.key(), *it.value());
        else
            request.environment.remove(it.key());
    }
    request.environment.insert(QStringLiteral("KETPLUS_SNAPSHOT_ID"), spec.snapshotId);
    request.environment.insert(QStringLiteral("KETPLUS_SOURCE_HEAD"), spec.sourceHead);
    return request;
}

// What the desktop learns again when it repeats a spec this runner already took.
void JobSession::answerCurrentState() {
    const JobAssignment& job = m_job->assignment;
    if (m_phase == Phase::Starting)
        return; // `job.accepted` or `job.rejected` follows the Relay's answer
    send(JobAccepted{job.jobId, job.attempt});
    if (m_job->launched)
        send(JobStarted{job.jobId, m_job->startedAt});
    if (!m_job->result)
        return;
    if (m_streaming) { // after the output this connection asked for
        m_resultPending = true;
        flush();
    } else {
        sendResult();
    }
}

} // namespace ketplus::runner::session
