#include "runner/peer/JobPeerSession.h"
#include "runner/peer/PeerLog.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace ketplus::daemon::runner::peer {

using namespace protocol;

namespace {

const QString userReason = QStringLiteral("user");
const QString sessionClosedReason = QStringLiteral("session_closed");
const QString snapshotStop = QStringLiteral("snapshot_failed");
const QString storageError = QStringLiteral("storage_error");

// A `snapshot.error` after which the runner sends no more snapshot frames for the pack.
bool endsSnapshot(const SnapshotError& error) {
    return error.code != QLatin1StringView("chunk_corrupt") &&
           error.code != QLatin1StringView("base_missing");
}

} // namespace

// After `welcome`, by the record: the spec to a runner without the mark (dispatched first), the
// same spec again to the runner holding the mark before `job.accepted`, `job.resume` after it.
void JobPeerSession::beginJob() {
    const std::optional<RecordView> view = ports_.records.view(jobId_);
    const QString connectionId = open_->connectionId;
    const QString runnerId = open_->runnerId;
    if (!view) {
        closeLink(connectionId, QString(toString(ErrorCode::JobMismatch)), true);
        return;
    }
    if (view->dispatchedTo.isEmpty()) {
        if (sendSpec(true))
            sendCancelIfStopping();
        return;
    }
    if (view->dispatchedTo != runnerId) {
        closeLink(connectionId, QString(toString(ErrorCode::AlreadyDispatched)), true);
        return;
    }
    if (!view->accepted) {
        if (sendSpec(false))
            sendCancelIfStopping();
        return;
    }
    intake_.reconnected();
    spoolBlocked_ = false;
    ports_.clock.disarm(jobId_, PeerTimer::SpoolRetry);
    if (phase_ == Phase::Waiting || phase_ == Phase::Accepting)
        phase_ = view->started ? Phase::Running : Phase::Syncing;
    send(JobResume{0, jobId_, intake_.size()});
    sendCancelIfStopping();
}

// Sends the job's spec, after the record's dispatch when `dispatch` is set. False when the
// connection was closed instead.
bool JobPeerSession::sendSpec(const bool dispatch) {
    const QString connectionId = open_->connectionId;
    const std::optional<JobSpec> spec = ports_.specs.spec(jobId_, open_->attempt);
    if (!spec || spec->jobId != jobId_ || spec->attempt != open_->attempt ||
        validate(*spec, Direction::DesktopToRunner)) {
        closeLink(connectionId, QStringLiteral("spec_unavailable"), true);
        return false;
    }
    if (dispatch) {
        const RecordAnswer answer = ports_.records.dispatch(jobId_, open_->runnerId);
        if (!answer.ok()) {
            closeLink(connectionId, answer.code, true);
            return false;
        }
    }
    phase_ = Phase::Accepting;
    send(*spec);
    return true;
}

void JobPeerSession::onFrame(Link& link, const Frame& frame) {
    std::visit(
        [this, &link](const auto& f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, Hello>) {
                closeLink(link.connectionId, QStringLiteral("unexpected_hello"), true);
            } else if constexpr (std::is_same_v<T, JobAccepted>) {
                if (f.jobId == jobId_ && f.attempt == link.attempt)
                    onAccepted(f);
            } else if constexpr (std::is_same_v<T, JobRejected>) {
                if (f.jobId == jobId_ && f.attempt == link.attempt)
                    onRejected(f);
            } else if constexpr (std::is_same_v<T, JobStarted>) {
                if (f.jobId == jobId_)
                    onStarted(f);
            } else if constexpr (std::is_same_v<T, JobOutput>) {
                if (f.jobId == jobId_)
                    onOutput(f);
            } else if constexpr (std::is_same_v<T, JobOutputGap>) {
                if (f.jobId == jobId_)
                    onGap(f);
            } else if constexpr (std::is_same_v<T, JobResult>) {
                if (f.jobId == jobId_ && f.attempt == link.attempt)
                    onResult(f);
            } else if constexpr (std::is_same_v<T, SnapshotWant> ||
                                 std::is_same_v<T, SnapshotAck> ||
                                 std::is_same_v<T, SnapshotReady>) {
                if (snapshotRunning_)
                    ports_.snapshots.receive(jobId_, f);
            } else if constexpr (std::is_same_v<T, SnapshotError>) {
                if (snapshotRunning_) {
                    ports_.snapshots.receive(jobId_, f);
                    if (endsSnapshot(f))
                        stopSnapshot(); // the runner reports the job's end itself
                }
            } else if constexpr (std::is_same_v<T, Ping>) {
                send(Pong{std::nullopt, f.nonce});
            } else if constexpr (std::is_same_v<T, Close>) {
                dropLink(link.connectionId);
            }
            // Anything else the runner may send has no meaning here (`pong`).
        },
        frame);
}

void JobPeerSession::onAccepted(const JobAccepted&) {
    if (phase_ == Phase::Accepting) {
        phase_ = Phase::Syncing;
        ports_.records.accepted(jobId_, open_->runnerId);
    } else if (phase_ != Phase::Syncing || snapshotRunning_) {
        return;
    }
    // Also on a resumed connection: the snapshot goes on with its `hello`.
    const std::optional<RecordView> view = ports_.records.view(jobId_);
    if (view && view->stopReason.isEmpty() && !view->terminal)
        startSnapshot();
}

void JobPeerSession::onRejected(const JobRejected& rejected) {
    stopSnapshot();
    if (phase_ != Phase::Accepting)
        return;
    phase_ = Phase::Waiting; // the record decides whether another runner may take the job
    ports_.records.rejected(jobId_, open_->runnerId, rejected.code);
}

void JobPeerSession::onStarted(const JobStarted&) {
    if (phase_ != Phase::Accepting && phase_ != Phase::Syncing)
        return;
    phase_ = Phase::Running;
    ports_.records.started(jobId_, open_->runnerId);
}

void JobPeerSession::onResult(const JobResult& result) {
    if (phase_ == Phase::Waiting)
        return;
    stopSnapshot();
    result_ = result; // a repeat after the ack was lost is answered again by the record
    resultRunner_ = open_->runnerId;
    retryMs_ = firstRetryMs;
    recordResult();
}

// Hands the held result to the record; `job.result.ack` only once it is durable. A refused write
// is tried again, also without a connection.
void JobPeerSession::recordResult() {
    if (!result_)
        return;
    const RecordAnswer answer = ports_.records.result(jobId_, resultRunner_, *result_);
    if (answer.code == storageError) {
        retryLater();
        return;
    }
    result_.reset();
    phase_ = Phase::Finished;
    if (!answer.ok())
        return; // the record does not take this runner's result
    if (const std::optional<qint64> offset = intake_.ackDue(); offset && !spoolBlocked_)
        sendAck(*offset);
    send(JobResultAck{0, jobId_});
}

void JobPeerSession::retryLater() {
    ports_.clock.arm(jobId_, PeerTimer::Retry, retryMs_);
    retryMs_ = std::min(retryMs_ * 2, maxRetryMs);
}

void JobPeerSession::startSnapshot() {
    const QString connectionId = open_->connectionId;
    snapshotRunning_ = true;
    snapshotRunner_ = open_->runnerId;
    ports_.snapshots.start(
        jobId_, open_->runnerId, open_->held,
        [this, connectionId](const Frame& frame) {
            if (snapshotRunning_ && open_ && open_->connectionId == connectionId)
                send(frame);
        },
        [this, connectionId](const QString& transferId, const QByteArray& bytes) {
            if (snapshotRunning_ && open_ && open_->connectionId == connectionId)
                sendBulk(transferId, bytes);
        });
}

void JobPeerSession::stopSnapshot() {
    if (!snapshotRunning_)
        return;
    snapshotRunning_ = false;
    ports_.snapshots.cancel(jobId_);
}

void JobPeerSession::snapshotReady() { snapshotRunning_ = false; }

// The desktop could not send the snapshot: the record stops the job and the runner is told to
// end it.
void JobPeerSession::snapshotFailed(const QString& code) {
    if (!snapshotRunning_)
        return;
    snapshotRunning_ = false;
    snapshotFailure_ = code;
    retryMs_ = firstRetryMs;
    reportSnapshotFailure();
}

void JobPeerSession::reportSnapshotFailure() {
    if (!snapshotFailure_)
        return;
    const RecordAnswer answer =
        ports_.records.snapshotFailed(jobId_, snapshotRunner_, *snapshotFailure_);
    if (answer.code == storageError) {
        retryLater();
        return;
    }
    snapshotFailure_.reset();
    if (answer.ok() && open_ && open_->runnerId == snapshotRunner_)
        sendCancel(sessionClosedReason);
}

void JobPeerSession::cancel(const QString& runnerId) {
    if (!open_ || open_->runnerId != runnerId || phase_ == Phase::Finished)
        return;
    stopSnapshot();
    sendCancel(userReason);
}

// One `job.cancel` per connection.
void JobPeerSession::sendCancel(const QString& reason) {
    if (!open_ || open_->cancelSent)
        return;
    open_->cancelSent = true;
    send(JobCancel{0, jobId_, reason});
}

// A new connection to a job the record is stopping gets its cancel again; never for a deadline.
void JobPeerSession::sendCancelIfStopping() {
    const std::optional<RecordView> view = ports_.records.view(jobId_);
    if (!view || view->terminal || !open_)
        return;
    if (view->stopReason == userReason)
        cancel(open_->runnerId);
    else if (view->stopReason == snapshotStop)
        sendCancel(sessionClosedReason);
}

} // namespace ketplus::daemon::runner::peer
