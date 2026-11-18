#pragma once

#include "peer/PeerPorts.h"
#include "peer/RelayWakeups.h"
#include "run/Backoff.h"

#include <QJsonObject>
#include <QString>

#include <memory>

// The runner's `/v2/relay` socket: one socket at a time, ready only after `ready` with the
// runner role, reconnects on the runner's backoff, and hands wake-ups and `signal.*` messages
// to their listeners.
namespace ketplus::runner::peer {

inline constexpr qint64 relayReadyTimeoutMs = 10'000;
inline constexpr qint64 relayHealthyMs = 60'000;

class RelayLinkListener {
  public:
    virtual ~RelayLinkListener() = default;
    virtual void ready() = 0;
    // A ready socket closed; the link reconnects on its own.
    virtual void lost() = 0;
    // The upgrade was refused: the HTTP status, or 0 when unknown. The link retries.
    virtual void refused(int status) = 0;
    // The link stopped for good: the runner was removed.
    virtual void runnerRemoved() = 0;
    // The link stopped for good: the token source refused the runner's credential.
    virtual void tokenRejected() = 0;
    virtual void signal(const QJsonObject& message) = 0;
};

class RelayLink final : public RelaySocketEvents {
  public:
    RelayLink(RelaySocket& socket, TokenSource& tokens, PeerClock& clock, app::Backoff backoff,
              RelayLinkListener& listener, WakeupListener& wakeups);
    ~RelayLink() override;

    void start();
    // Closes the socket and stops retrying; a ready link reports lost() first.
    void stop();
    [[nodiscard]] bool isReady() const { return m_state == State::Ready; }
    // A message on the ready socket; false otherwise.
    bool send(const QJsonObject& message);

    void timerFired(PeerTimer timer);

    void socketOpened() override;
    void socketText(const QString& text) override;
    void socketClosed(int code) override;
    void socketRefused(int status) override;

  private:
    enum class State { Stopped, Token, Connecting, Ready, Waiting };

    void connectNow(bool renew);
    void tokenAnswered(quint64 generation, const TokenSource::Token& token);
    void retryAfter(qint64 delayMs, bool renew);
    void retryOnBackoff(bool renew);
    void halt();

    RelaySocket& m_socket;
    TokenSource& m_tokens;
    PeerClock& m_clock;
    app::Backoff m_backoff;
    RelayLinkListener& m_listener;
    WakeupListener& m_wakeups;

    State m_state = State::Stopped;
    quint64 m_generation = 0;
    bool m_renewNext = false;
    bool m_last401 = false;
    int m_failures = 0; // since the last ready socket or start()
    qint64 m_readyAt = 0;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};

} // namespace ketplus::runner::peer
