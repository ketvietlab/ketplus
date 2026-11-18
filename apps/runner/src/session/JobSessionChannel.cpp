#include "protocol/Capabilities.h"
#include "protocol/FrameRoutes.h"
#include "session/JobSession.h"

#include <type_traits>
#include <variant>

namespace ketplus::runner::session {

using namespace protocol;

JobSession::JobSession(RunnerInfo runner, SessionPorts ports)
    : m_runner(std::move(runner)), m_ports(ports) {}

bool JobSession::assign(const JobAssignment& job) {
    if (m_phase != Phase::Idle)
        return false;
    if (m_link != Link::None)
        closeChannel(QStringLiteral("job_finished")); // the previous job's connection
    ++m_generation;
    m_job = Job{};
    m_job->assignment = job;
    m_phase = Phase::Assigned;
    m_output.clear();
    m_sentUpTo = 0;
    m_ports.clock.arm(SessionTimer::PeerOpen, peerOpenMs);
    m_ports.channel.requestConnection();
    return true;
}

void JobSession::channelOpened(const QString& connectionId) {
    if (m_phase == Phase::Idle) {
        m_ports.channel.close(QStringLiteral("no_job"));
        return;
    }
    m_link = Link::AwaitingWelcome;
    m_connectionId = connectionId;
    m_streaming = false;
    m_ports.clock.disarm(SessionTimer::Reconnect);
    m_ports.clock.disarm(SessionTimer::PeerAbsent);
    sendHello();
}

void JobSession::sendHello() {
    const JobAssignment& job = m_job->assignment;
    Hello hello;
    hello.connectionId = m_connectionId;
    hello.capabilities = {QString(capability::runnerProtocol)};
    hello.runnerId = m_runner.runnerId;
    hello.runnerVersion = m_runner.runnerVersion;
    hello.os = m_runner.os;
    hello.arch = m_runner.arch;
    hello.labels = m_runner.labels;
    hello.jobId = job.jobId;
    hello.attempt = job.attempt;
    hello.snapshots = m_ports.snapshots.heldSnapshots(job.machineId);
    send(hello);
}

void JobSession::channelLost() {
    if (m_link != Link::None)
        linkLost();
}

void JobSession::closeChannel(const QString& reason) {
    m_ports.channel.close(reason);
    linkLost();
}

void JobSession::linkLost() {
    m_link = Link::None;
    m_streaming = false;
    m_resultPending = false;
    m_connectionId.clear();
    if (m_phase == Phase::Idle)
        return;
    m_ports.clock.arm(SessionTimer::Reconnect, firstReconnectMs);
    if (m_phase == Phase::Syncing || m_phase == Phase::Running)
        m_ports.clock.arm(SessionTimer::PeerAbsent, peerAbsentMs);
}

bool JobSession::send(const Frame& frame) {
    if (m_link == Link::None)
        return false;
    const EncodeResult encoded = encode(frame, Direction::RunnerToDesktop);
    const Route* route = routeFor(frameType(frame));
    if (!encoded.ok() || route == nullptr)
        return false; // a frame this runner built breaks a rule: never put it on the wire
    m_ports.channel.send(route->channel, encoded.message);
    return true;
}

void JobSession::reject(const QString& jobId, const qint64 attempt, const ErrorCode code) {
    send(JobRejected{jobId, attempt, QString(toString(code))});
}

bool JobSession::isCurrent(const QString& jobId) const {
    return m_job && m_job->assignment.jobId == jobId;
}

void JobSession::frameReceived(const Channel channel, const QByteArray& message) {
    if (m_link == Link::None)
        return;
    const DecodeResult decoded = decode(message, channel, Direction::DesktopToRunner);
    if (!decoded.ok()) {
        onBrokenFrame(channel, decoded.error);
        return;
    }
    const Frame& frame = *decoded.frame;
    if (m_link == Link::AwaitingWelcome && !std::holds_alternative<Welcome>(frame) &&
        !std::holds_alternative<Ping>(frame) && !std::holds_alternative<Close>(frame)) {
        closeChannel(QStringLiteral("frame_before_welcome"));
        return;
    }
    onFrame(frame);
}

void JobSession::onBrokenFrame(const Channel channel, const FrameError& error) {
    using Kind = FrameError::Kind;
    switch (error.kind) {
    case Kind::UnsupportedType:
        // Answered on `control` only; other channels ignore it.
        if (channel == Channel::Control && m_job)
            reject(m_job->assignment.jobId, m_job->assignment.attempt, ErrorCode::UnsupportedFrame);
        return;
    case Kind::InvalidField:
        if (error.type == JobSpec::type && error.jobId && error.attempt && m_link == Link::Open) {
            onInvalidSpec(*error.jobId, *error.attempt);
            return;
        }
        closeChannel(QStringLiteral("invalid_frame"));
        return;
    case Kind::Oversize:
        closeChannel(QStringLiteral("frame_too_large"));
        return;
    case Kind::WrongChannel:
    case Kind::WrongDirection:
        closeChannel(QStringLiteral("wrong_route"));
        return;
    case Kind::NotJson:
    case Kind::InvalidUtf8:
    case Kind::NotObject:
    case Kind::MissingType:
    case Kind::MissingCapability:
        closeChannel(QStringLiteral("invalid_frame"));
        return;
    }
}

void JobSession::onWelcome(const Welcome& welcome) {
    if (m_link != Link::AwaitingWelcome || welcome.connectionId != m_connectionId) {
        closeChannel(QStringLiteral("unexpected_welcome"));
        return;
    }
    if (!welcome.capabilities.contains(QString(capability::runnerProtocol))) {
        m_ports.channel.close(QStringLiteral("unsupported_protocol"));
        m_link = Link::None;
        if (m_phase == Phase::Assigned) {
            m_ports.lease.release(QStringLiteral("peer_unreachable"));
            endJob();
            return;
        }
        linkLost();
        return;
    }
    m_link = Link::Open;
    m_ports.clock.disarm(SessionTimer::PeerOpen);
    if (m_phase == Phase::Assigned && !m_job->welcomed)
        m_ports.clock.arm(SessionTimer::PeerOpen, peerOpenMs); // now for the spec
    m_job->welcomed = true;
    // Before a spec there is no output; after one, this is a new connection and the desktop says
    // where to go on with `job.resume`.
    m_streaming = m_phase == Phase::Assigned || m_phase == Phase::Starting;
}

void JobSession::onFrame(const Frame& frame) {
    std::visit(
        [this](const auto& f) {
            using T = std::decay_t<decltype(f)>;
            if constexpr (std::is_same_v<T, Welcome>) {
                onWelcome(f);
            } else if constexpr (std::is_same_v<T, JobSpec>) {
                onSpec(f);
            } else if constexpr (std::is_same_v<T, JobResume>) {
                onResume(f);
            } else if constexpr (std::is_same_v<T, JobOutputAck>) {
                if (isCurrent(f.jobId))
                    m_output.acknowledge(f.offset);
            } else if constexpr (std::is_same_v<T, JobOutputResume>) {
                if (isCurrent(f.jobId) && m_job->launched)
                    resumeOutput(f.fromOffset);
            } else if constexpr (std::is_same_v<T, JobCancel>) {
                if (isCurrent(f.jobId))
                    cancel(f.reason);
            } else if constexpr (std::is_same_v<T, JobResultAck>) {
                if (isCurrent(f.jobId))
                    onResultAck();
            } else if constexpr (std::is_same_v<T, SnapshotOffer>) {
                if (m_phase == Phase::Syncing && isCurrent(f.jobId))
                    m_ports.snapshots.receive(f);
            } else if constexpr (std::is_same_v<T, SnapshotChunk>) {
                if (m_phase == Phase::Syncing && f.snapshotId == m_job->spec->snapshotId)
                    m_ports.snapshots.receive(f);
            } else if constexpr (std::is_same_v<T, Ping>) {
                send(Pong{std::nullopt, f.nonce});
            } else if constexpr (std::is_same_v<T, Close>) {
                closeChannel(QStringLiteral("closed_by_peer"));
            }
            // `pong` needs nothing; the decoder already refused frames the desktop may not send.
        },
        frame);
}

} // namespace ketplus::runner::session
