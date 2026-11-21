#pragma once

#include "peer/PeerPorts.h"
#include "peer/PeerSignals.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <memory>
#include <optional>

// The peer connection of the current job: signaling attempts over the Relay socket, retried
// until one opens or the caller closes, and the open connection afterwards. One attempt or
// connection at a time.
namespace ketplus::runner::peer {

inline constexpr qint64 attemptDeadlineMs = 20'000;
inline constexpr qint64 attemptRetryBaseMs = 2'500;
inline constexpr qint64 attemptRetryCapMs = 15'000;
inline constexpr qint64 startSpacingMs = 2'500;

struct PeerRequest {
    QString jobId;
    QString leaseId;
    int attempt = 0;
    QString machineId; // the desktop that submitted the job
};

// How signaling sends: the Relay socket when it is ready.
class SignalSender {
  public:
    virtual ~SignalSender() = default;
    [[nodiscard]] virtual bool relayReady() const = 0;
    virtual bool sendSignal(const QJsonObject& message) = 0;
};

class ConnectorEvents {
  public:
    virtual ~ConnectorEvents() = default;
    // The three channels are open. Once per connection.
    virtual void connectionOpened(const QString& connectionId) = 0;
    // The open connection ended other than by close(). Once per connection.
    virtual void connectionLost(const QString& reason) = 0;
    // An attempt ended; another follows.
    virtual void attemptFailed(const QString& reason) = 0;
    // The request ended without a connection; no attempt follows.
    virtual void gaveUp(const QString& reason) = 0;
    virtual void received(Channel channel, const QByteArray& bytes, bool binary) = 0;
    virtual void writable() = 0;
};

class PeerConnector final : public PeerTransportEvents {
  public:
    PeerConnector(SignalSender& sender, PeerClock& clock, PeerTransportFactory& transports,
                  const OfferSigner& signer, const PinLookup& pins, ConnectorEvents& events);
    ~PeerConnector() override;

    // Ends any attempt or connection (an open one reports connectionLost) and starts anew.
    void request(const PeerRequest& request);
    // Ends the attempt or connection without reporting it.
    void close(const QString& reason);
    // Closes the open connection and reports connectionLost(reason).
    void fail(const QString& reason);
    [[nodiscard]] bool isOpen() const { return m_state == State::Open; }
    // The open connection's transport, or null.
    [[nodiscard]] PeerTransport* openTransport() const;

    void relayReady();
    void relayLost();
    void runnerRemoved();
    void signal(const QJsonObject& message);
    void timerFired(PeerTimer timer);

    void localDescription(const QString& sdp) override;
    void localCandidate(const QString& candidate, const QString& mid) override;
    void opened() override;
    void received(Channel channel, const QByteArray& bytes, bool binary) override;
    void writable() override;
    void closed() override;

  private:
    enum class State {
        Idle,       // no request
        WaitRelay,  // the Relay socket is not ready
        WaitRetry,  // the next start is due on AttemptRetry
        Starting,   // signal.start sent
        Offering,   // creating the offer
        Answering,  // signal.offer sent, waiting for the answer
        Connecting, // answer applied, waiting for the channels
        Open,
    };
    struct RemoteCandidate {
        QString candidate; // empty: end of candidates
        QString mid;
    };

    void scheduleStart(qint64 delayMs);
    void sendStart();
    void started(const QJsonObject& message);
    void answer(const QJsonObject& message);
    void remoteCandidate(const QJsonObject& message);
    void signalError(const QJsonObject& message);
    void remoteEnd(const QJsonObject& message);
    bool sendLocalCandidate(const QString& candidate, const QString& mid);
    void endAttempt(const QString& reason, bool retry, bool tellRelay = true);
    void dropTransport();
    [[nodiscard]] bool attempting() const;
    [[nodiscard]] qint64 nowMs() const;

    SignalSender& m_sender;
    PeerClock& m_clock;
    PeerTransportFactory& m_transports;
    const OfferSigner& m_signer;
    const PinLookup& m_pins;
    ConnectorEvents& m_events;

    State m_state = State::Idle;
    std::optional<PeerRequest> m_request;
    qint64 m_retryDelayMs = attemptRetryBaseMs;
    std::optional<qint64> m_lastStartMs;
    std::unique_ptr<PeerTransport> m_transport;
    OfferContext m_context;
    QJsonArray m_iceServers;
    QString m_offerSdp;
    bool m_offerSent = false;
    QList<RemoteCandidate> m_remoteCandidates; // until the answer is applied
    QList<RemoteCandidate> m_localCandidates;  // until the offer is sent
    int m_remoteCount = 0;
    bool m_remoteComplete = false;
    int m_localCount = 0;
    UsedAnswers m_usedAnswers;
};

} // namespace ketplus::runner::peer
