#include "runner/peer/JobPeerSession.h"
#include "runner/peer/PeerLog.h"

namespace ketplus::daemon::runner::peer {

using namespace protocol;

namespace {

bool takesOutput(const JobPeerSession::Phase phase) {
    return phase == JobPeerSession::Phase::Accepting || phase == JobPeerSession::Phase::Syncing ||
           phase == JobPeerSession::Phase::Running;
}

} // namespace

void JobPeerSession::onOutput(const JobOutput& output) {
    if (takesOutput(phase_) && !spoolBlocked_)
        applyStep(intake_.page(output.offset, output.data));
}

void JobPeerSession::onGap(const JobOutputGap& gap) {
    if (takesOutput(phase_) && !spoolBlocked_)
        applyStep(intake_.gap(gap.fromOffset, gap.toOffset));
}

void JobPeerSession::applyStep(const OutputStep& step) {
    if (step.kind == OutputStep::Kind::Failed) {
        // Nothing unwritten is ever acknowledged. Output is dropped until the spool is asked
        // again; the connection and the result stay.
        spoolBlocked_ = true;
        if (!spoolReported_) {
            spoolReported_ = true;
            qCWarning(ketplusRunnerPeer).noquote()
                << "job" << jobId_ << "spool write failed at offset" << intake_.size();
        }
        ports_.clock.arm(jobId_, PeerTimer::SpoolRetry, spoolRetryMs);
        return;
    }
    if (step.kind == OutputStep::Kind::Written)
        spoolReported_ = false;
    if (step.resumeFrom)
        send(JobOutputResume{0, jobId_, *step.resumeFrom});
    if (step.ackAt) {
        sendAck(*step.ackAt);
    } else if (step.armAckTimer && !ackArmed_) {
        ackArmed_ = true;
        ports_.clock.arm(jobId_, PeerTimer::OutputAck, outputAckMs);
    }
}

// Asks the runner again from the spool's offset; the next page tries the spool again.
void JobPeerSession::retrySpool() {
    if (!spoolBlocked_ || !open_)
        return;
    spoolBlocked_ = false;
    send(JobOutputResume{0, jobId_, intake_.resumeFromSpool()});
}

void JobPeerSession::sendAck(const qint64 offset) {
    if (ackArmed_) {
        ackArmed_ = false;
        ports_.clock.disarm(jobId_, PeerTimer::OutputAck);
    }
    send(JobOutputAck{0, jobId_, offset});
}

} // namespace ketplus::daemon::runner::peer
