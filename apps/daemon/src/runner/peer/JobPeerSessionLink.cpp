#include "protocol/Capabilities.h"
#include "runner/peer/JobPeerSession.h"
#include "runner/peer/PeerLog.h"

#include <utility>
#include <variant>

namespace ketplus::daemon::runner::peer {

using namespace protocol;

JobPeerSession::JobPeerSession(QString jobId, QString daemonVersion, SessionPorts ports)
    : jobId_(std::move(jobId)), daemonVersion_(std::move(daemonVersion)), ports_(ports),
      intake_(ports.spool) {}

QString JobPeerSession::openConnection() const { return open_ ? open_->connectionId : QString(); }

bool JobPeerSession::owns(const QString& connectionId) const {
    return (open_ && open_->connectionId == connectionId) ||
           (pending_ && pending_->connectionId == connectionId);
}

JobPeerSession::Link* JobPeerSession::linkOf(const QString& connectionId) {
    if (open_ && open_->connectionId == connectionId)
        return &*open_;
    if (pending_ && pending_->connectionId == connectionId)
        return &*pending_;
    return nullptr;
}

RecordAnswer JobPeerSession::offered(const QString& connectionId, const QString& runnerId,
                                     const qint64 attempt) {
    if (connectionId.isEmpty() || owns(connectionId))
        return {QString(toString(ErrorCode::JobMismatch))};
    const RecordAnswer answer = ports_.records.offer(jobId_, runnerId, attempt);
    if (answer.code == QLatin1StringView("storage_error"))
        return {QString(toString(ErrorCode::JobMismatch))}; // the runner offers again
    if (!answer.ok())
        return answer;
    if (pending_)
        closeLink(pending_->connectionId, QStringLiteral("superseded"), false);
    Link link;
    link.connectionId = connectionId;
    link.runnerId = runnerId;
    link.attempt = attempt;
    pending_ = std::move(link);
    return {};
}

void JobPeerSession::offerEnded(const QString& connectionId) {
    if (pending_ && pending_->connectionId == connectionId && pending_->state == LinkState::Offered)
        pending_.reset();
}

void JobPeerSession::channelsOpened(const QString& connectionId) {
    if (!pending_ || pending_->connectionId != connectionId ||
        pending_->state != LinkState::Offered)
        return;
    pending_->state = LinkState::AwaitingHello;
    ports_.clock.arm(jobId_, PeerTimer::Hello, helloTimeoutMs);
}

void JobPeerSession::frameReceived(const QString& connectionId, const Channel channel,
                                   const QByteArray& message) {
    Link* link = linkOf(connectionId);
    if (link == nullptr || link->state == LinkState::Offered)
        return; // a connection the session closed, or channels not reported open
    const DecodeResult decoded = decode(message, channel, Direction::RunnerToDesktop);
    if (!decoded.ok()) {
        onBrokenFrame(*link, decoded.error);
        return;
    }
    link->lastContact = ports_.clock.now();
    const Frame& frame = *decoded.frame;
    if (link->state == LinkState::AwaitingHello) {
        // Before `hello` only `hello` and `close` are taken.
        if (const auto* hello = std::get_if<Hello>(&frame))
            onHello(*link, *hello);
        else if (std::holds_alternative<Close>(frame))
            dropLink(connectionId);
        else
            closeLink(connectionId, QStringLiteral("frame_before_hello"), false);
        return;
    }
    onFrame(*link, frame);
}

void JobPeerSession::onHello(Link& link, const Hello& hello) {
    const QString connectionId = link.connectionId;
    if (hello.connectionId != link.connectionId || hello.runnerId != link.runnerId ||
        hello.jobId != jobId_ || hello.attempt != link.attempt) {
        closeLink(connectionId, QStringLiteral("hello_mismatch"), false);
        return;
    }
    const std::optional<RecordView> view = ports_.records.view(jobId_);
    if (!view) {
        closeLink(connectionId, QString(toString(ErrorCode::JobMismatch)), false);
        return;
    }
    for (const QString& label : view->runsOn) {
        if (!hello.labels.contains(label)) {
            closeLink(connectionId, QString(toString(ErrorCode::LabelMismatch)), false);
            return;
        }
    }
    ports_.clock.disarm(jobId_, PeerTimer::Hello);
    if (open_) {
        const QString previous = open_->connectionId;
        stopSnapshot();
        ports_.clock.disarm(jobId_, PeerTimer::OutputAck);
        ackArmed_ = false;
        ports_.links.close(previous, QStringLiteral("superseded"));
        open_.reset();
    }
    Link welcomed = std::move(*pending_);
    pending_.reset();
    welcomed.state = LinkState::Open;
    welcomed.epoch = ++epoch_;
    welcomed.held = hello.snapshots;
    open_ = std::move(welcomed);

    Welcome welcome;
    welcome.connectionId = connectionId;
    welcome.capabilities = {QString(capability::runnerProtocol)};
    welcome.daemonVersion = daemonVersion_;
    send(welcome);
    ports_.records.peerOpened(jobId_, open_->runnerId);
    beginJob();
}

void JobPeerSession::onBrokenFrame(Link& link, const FrameError& error) {
    using Kind = FrameError::Kind;
    QString reason;
    switch (error.kind) {
    case Kind::UnsupportedType:
        return; // a newer runner's frame: nothing to answer on this side
    case Kind::Oversize:
        reason = QStringLiteral("frame_too_large");
        break;
    case Kind::WrongChannel:
    case Kind::WrongDirection:
        reason = QStringLiteral("wrong_route");
        break;
    case Kind::MissingCapability:
        reason = QStringLiteral("unsupported_protocol");
        break;
    case Kind::NotJson:
    case Kind::InvalidUtf8:
    case Kind::NotObject:
    case Kind::MissingType:
    case Kind::InvalidField:
        reason = QStringLiteral("invalid_frame");
        break;
    }
    closeLink(link.connectionId, reason, link.state == LinkState::Open);
}

// Closes a connection of the session. `lost` reports a welcomed one lost to the record.
void JobPeerSession::closeLink(const QString& connectionId, const QString& reason,
                               const bool lost) {
    qCInfo(ketplusRunnerPeer).noquote()
        << "job" << jobId_ << "connection" << connectionId << "closed:" << reason;
    ports_.links.close(connectionId, reason);
    if (lost) {
        if (open_ && open_->connectionId == connectionId) {
            linkLost(*open_);
            return;
        }
    }
    dropLink(connectionId);
}

// Forgets a connection without reporting it lost (never welcomed, or replaced).
void JobPeerSession::dropLink(const QString& connectionId) {
    if (pending_ && pending_->connectionId == connectionId) {
        if (pending_->state == LinkState::AwaitingHello)
            ports_.clock.disarm(jobId_, PeerTimer::Hello);
        pending_.reset();
        return;
    }
    if (open_ && open_->connectionId == connectionId)
        linkLost(*open_);
}

void JobPeerSession::close() {
    stopSnapshot();
    for (const PeerTimer timer :
         {PeerTimer::Hello, PeerTimer::OutputAck, PeerTimer::Retry, PeerTimer::SpoolRetry})
        ports_.clock.disarm(jobId_, timer);
    ackArmed_ = false;
    result_.reset();
    snapshotFailure_.reset();
    phase_ = Phase::Finished;
    if (pending_)
        ports_.links.close(std::exchange(pending_, std::nullopt)->connectionId,
                           QStringLiteral("session_closed"));
    if (open_)
        ports_.links.close(std::exchange(open_, std::nullopt)->connectionId,
                           QStringLiteral("session_closed"));
}

void JobPeerSession::connectionLost(const QString& connectionId) { dropLink(connectionId); }

void JobPeerSession::linkLost(Link& link) {
    const QString runnerId = link.runnerId;
    const QDateTime lastContact = link.lastContact;
    stopSnapshot();
    ports_.clock.disarm(jobId_, PeerTimer::OutputAck);
    ports_.clock.disarm(jobId_, PeerTimer::SpoolRetry);
    ackArmed_ = false;
    open_.reset();
    ports_.records.peerLost(jobId_, runnerId, lastContact);
}

bool JobPeerSession::send(const Frame& frame) {
    if (!open_)
        return false;
    Frame stamped = frame;
    std::visit(
        [this](auto& f) {
            if constexpr (requires { f.epoch; })
                f.epoch = epoch_;
        },
        stamped);
    const EncodeResult encoded = encode(stamped, Direction::DesktopToRunner);
    const Route* route = routeFor(frameType(stamped));
    if (!encoded.ok() || route == nullptr)
        return false; // a frame this daemon built breaks a rule: never put it on the wire
    ports_.links.send(open_->connectionId, route->channel, encoded.message);
    return true;
}

void JobPeerSession::sendBulk(const QString& transferId, const QByteArray& bytes) {
    if (open_)
        ports_.links.sendBulk(open_->connectionId, transferId, bytes);
}

void JobPeerSession::bulkWritable(const QString& connectionId) {
    if (snapshotRunning_ && open_ && open_->connectionId == connectionId)
        ports_.snapshots.bulkWritable(jobId_);
}

void JobPeerSession::timerFired(const PeerTimer timer) {
    switch (timer) {
    case PeerTimer::Hello:
        if (pending_ && pending_->state == LinkState::AwaitingHello)
            closeLink(pending_->connectionId, QStringLiteral("hello_timeout"), false);
        return;
    case PeerTimer::OutputAck:
        ackArmed_ = false;
        if (spoolBlocked_)
            return;
        if (const std::optional<qint64> offset = intake_.ackDue())
            sendAck(*offset);
        return;
    case PeerTimer::Retry:
        recordResult();
        reportSnapshotFailure();
        return;
    case PeerTimer::SpoolRetry:
        retrySpool();
        return;
    }
}

} // namespace ketplus::daemon::runner::peer
