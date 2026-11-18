#pragma once

#include "pairing/PairingPins.h"
#include "protocol/FrameRoutes.h"
#include "runner/peer/PeerPorts.h"
#include "transport/FrameCodec.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <memory>
#include <optional>

// The ports of the desktop's runner peer links. The links answer runner offers relayed by the
// Relay and carry the three channels of each connection; every effect goes through these ports.
namespace ketplus::daemon::runner::link {

using TransportChannel = transport::Channel;
using FrameChannel = ketplus::runner::protocol::Channel;

// Time for the links: one deadline per connection id. Answer: RunnerLinks::timerFired. Arming an
// armed deadline restarts it.
class LinkClock {
  public:
    virtual ~LinkClock() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
    virtual void arm(const QString& connectionId, qint64 delayMs) = 0;
    virtual void disarm(const QString& connectionId) = 0;
};

// Answers of one peer transport. The links ignore answers of a transport they dropped.
class LinkTransportEvents {
  public:
    virtual ~LinkTransportEvents() = default;
    virtual void localDescription(const QString& sdp) = 0;
    // An empty candidate ends local gathering.
    virtual void localCandidate(const QString& candidate, const QString& mid) = 0;
    // The three channels are open.
    virtual void opened() = 0;
    virtual void received(TransportChannel channel, const QByteArray& bytes, bool binary) = 0;
    virtual void writable() = 0;
    // Closed by the other side, by ICE or by DTLS; never after close().
    virtual void closed() = 0;
};

// One WebRTC peer connection in the answering role. The DTLS certificate of the other side must
// match the fingerprint of the offer given to answer().
class LinkTransport {
  public:
    virtual ~LinkTransport() = default;
    // Applies the offer and creates the answer (localDescription).
    virtual bool answer(const QString& offerSdp, const QJsonArray& iceServers) = 0;
    virtual bool candidate(const QString& candidate, const QString& mid) = 0;
    virtual bool send(TransportChannel channel, const QByteArray& bytes, bool binary) = 0;
    [[nodiscard]] virtual quint64 buffered(TransportChannel channel) const = 0;
    // Closes without a later closed().
    virtual void close() = 0;
};

class LinkTransportFactory {
  public:
    virtual ~LinkTransportFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<LinkTransport> create(LinkTransportEvents& events) = 0;
};

// This machine: what answers name and the machine key that signs them.
class MachineIdentity {
  public:
    virtual ~MachineIdentity() = default;
    [[nodiscard]] virtual QString workspaceId() const = 0;
    [[nodiscard]] virtual QString machineId() const = 0;
    [[nodiscard]] virtual int machineKeyVersion() const = 0;
    // 64-byte Ed25519 signature, or empty on failure.
    [[nodiscard]] virtual QByteArray sign(const QByteArray& bytes) const = 0;
};

// The runners this machine trusts: the pin of a runner, or nothing.
class RunnerPins {
  public:
    virtual ~RunnerPins() = default;
    [[nodiscard]] virtual std::optional<ketplus::runner::pairing::DesktopRunnerPin>
    pin(const QString& runnerId) const = 0;
};

// How signaling leaves: one `signal.*` message to the Relay.
class SignalOut {
  public:
    virtual ~SignalOut() = default;
    virtual void sendSignal(const QJsonObject& message) = 0;
};

using RecordAnswer = peer::RecordAnswer;

// The job side of the links, named and typed as the entry points of the desktop's peer sessions.
class LinkEvents {
  public:
    virtual ~LinkEvents() = default;
    // A verified offer. An ok answer takes it; any other code refuses it and ends signaling
    // with that reason. Asked once per connection.
    [[nodiscard]] virtual RecordAnswer offered(const QString& connectionId, const QString& runnerId,
                                               const QString& jobId, qint64 attempt) = 0;
    // A taken offer ended before its channels opened. Once per connection.
    virtual void offerEnded(const QString& connectionId) = 0;
    // The three channels are open. Once per connection.
    virtual void channelsOpened(const QString& connectionId) = 0;
    // One message received on `control` or `events`, exactly as it arrived.
    virtual void frameReceived(const QString& connectionId, FrameChannel channel,
                               const QByteArray& message) = 0;
    // `bulk` takes transfers again after bulkWritable() answered false. Once per such answer.
    virtual void bulkWritable(const QString& connectionId) = 0;
    // The open connection ended other than by close() or by a newer connection of the same
    // runner. Once per connection; the reason is logged just before.
    virtual void connectionLost(const QString& connectionId) = 0;
};

} // namespace ketplus::daemon::runner::link
