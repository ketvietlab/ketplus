#pragma once

#include "pairing/PairingPins.h"
#include "transport/FrameCodec.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

// What the runner's Relay socket and peer connection need from the rest of the process. The
// logic in this directory opens no socket, starts no peer connection, reads no clock and no
// file: every effect goes through these interfaces, and every answer comes back as a call on
// the logic object that owns the port.
namespace ketplus::runner::peer {

using Channel = daemon::transport::Channel;

enum class PeerTimer {
    RelayRetry,      // reconnect the Relay socket
    RelayReady,      // `ready` must follow the socket's opening
    AttemptDeadline, // a signaling attempt must open its three channels
    AttemptRetry,    // start the next signaling attempt
};

// Time for the Relay link and the connector. Answer: the owner's timerFired. Arming an armed
// timer restarts it.
class PeerClock {
  public:
    virtual ~PeerClock() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
    virtual void arm(PeerTimer timer, qint64 delayMs) = 0;
    virtual void disarm(PeerTimer timer) = 0;
};

// Answers of the `/v2/relay` WebSocket.
class RelaySocketEvents {
  public:
    virtual ~RelaySocketEvents() = default;
    virtual void socketOpened() = 0;
    virtual void socketText(const QString& text) = 0;
    // The open socket closed, with its close code (1006 when none was received).
    virtual void socketClosed(int code) = 0;
    // The upgrade was refused before the socket opened: the HTTP status, or 0 when unknown.
    virtual void socketRefused(int status) = 0;
};

// The `/v2/relay` WebSocket. At most one socket exists; open() while one exists replaces it
// without answers for the old one.
class RelaySocket {
  public:
    virtual ~RelaySocket() = default;
    virtual void open(const QString& accessToken) = 0;
    // One text message; false when the socket is not open or the message is too large.
    virtual bool send(const QJsonObject& message) = 0;
    // Closes without later answers.
    virtual void close() = 0;
};

// The runner's access token. Answers through the callback, possibly at once.
class TokenSource {
  public:
    enum class Outcome {
        Ok,
        Retry,         // try again after a delay
        InvalidAccess, // the refresh credential is refused for good
        RunnerRemoved,
    };
    struct Token {
        Outcome outcome = Outcome::Retry;
        QString value; // Ok only
    };
    using Answer = std::function<void(const Token&)>;
    virtual ~TokenSource() = default;
    // A token with time left, renewed when needed.
    virtual void current(Answer answer) = 0;
    // A new token even when the current one has time left.
    virtual void renew(Answer answer) = 0;
};

// Answers of one peer transport. The connector ignores answers of a transport it dropped.
class PeerTransportEvents {
  public:
    virtual ~PeerTransportEvents() = default;
    virtual void localDescription(const QString& sdp) = 0;
    // An empty candidate ends local gathering.
    virtual void localCandidate(const QString& candidate, const QString& mid) = 0;
    // The three channels are open.
    virtual void opened() = 0;
    virtual void received(Channel channel, const QByteArray& bytes, bool binary) = 0;
    virtual void writable() = 0;
    // Closed by the other side, by ICE or by DTLS; never after close().
    virtual void closed() = 0;
};

// One WebRTC peer connection in the offerer role.
class PeerTransport {
  public:
    virtual ~PeerTransport() = default;
    // Creates the control, events and bulk channels and the offer (localDescription).
    virtual bool offer(const QJsonArray& iceServers) = 0;
    // `fingerprint` comes from the signed answer: the DTLS certificate must match it.
    virtual bool acceptAnswer(const QString& sdp, const QString& fingerprint) = 0;
    virtual bool candidate(const QString& candidate, const QString& mid) = 0;
    virtual bool send(Channel channel, const QByteArray& bytes, bool binary) = 0;
    [[nodiscard]] virtual quint64 buffered(Channel channel) const = 0;
    // Closes without a later closed().
    virtual void close() = 0;
};

class PeerTransportFactory {
  public:
    virtual ~PeerTransportFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<PeerTransport> create(PeerTransportEvents& events) = 0;
};

// The runner key that signs offers.
class OfferSigner {
  public:
    virtual ~OfferSigner() = default;
    [[nodiscard]] virtual QString runnerId() const = 0;
    [[nodiscard]] virtual int runnerKeyVersion() const = 0;
    // 64-byte Ed25519 signature, or empty on failure.
    [[nodiscard]] virtual QByteArray sign(const QByteArray& bytes) const = 0;
};

// The runner's pins: the active pin of a desktop, or nothing.
class PinLookup {
  public:
    virtual ~PinLookup() = default;
    [[nodiscard]] virtual std::optional<pairing::RunnerPin>
    activePin(const QString& machineId) const = 0;
};

} // namespace ketplus::runner::peer
