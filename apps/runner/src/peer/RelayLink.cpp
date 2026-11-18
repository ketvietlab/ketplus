#include "peer/RelayLink.h"

#include "peer/PeerLog.h"

#include <QJsonDocument>

namespace ketplus::runner::peer {
namespace {

constexpr int closeCredentialExpired = 4001;
constexpr int closeRunnerRemoved = 4003;
constexpr int statusUnauthorized = 401;

} // namespace

RelayLink::RelayLink(RelaySocket& socket, TokenSource& tokens, PeerClock& clock,
                     app::Backoff backoff, RelayLinkListener& listener, WakeupListener& wakeups)
    : m_socket(socket), m_tokens(tokens), m_clock(clock), m_backoff(std::move(backoff)),
      m_listener(listener), m_wakeups(wakeups) {}

RelayLink::~RelayLink() {
    if (m_state != State::Stopped)
        halt();
}

void RelayLink::start() {
    if (m_state != State::Stopped)
        return;
    m_backoff.reset();
    m_failures = 0;
    m_last401 = false;
    connectNow(false);
}

void RelayLink::stop() {
    if (m_state == State::Stopped)
        return;
    const bool wasReady = m_state == State::Ready;
    halt();
    if (wasReady)
        m_listener.lost();
}

bool RelayLink::send(const QJsonObject& message) {
    return m_state == State::Ready && m_socket.send(message);
}

void RelayLink::halt() {
    m_state = State::Stopped;
    ++m_generation;
    m_clock.disarm(PeerTimer::RelayRetry);
    m_clock.disarm(PeerTimer::RelayReady);
    m_socket.close();
}

void RelayLink::connectNow(const bool renew) {
    m_state = State::Token;
    m_renewNext = false;
    const quint64 generation = ++m_generation;
    auto answer = [this, generation,
                   alive = std::weak_ptr<int>(m_lifetime)](const TokenSource::Token& token) {
        if (!alive.expired())
            tokenAnswered(generation, token);
    };
    if (renew)
        m_tokens.renew(std::move(answer));
    else
        m_tokens.current(std::move(answer));
}

void RelayLink::tokenAnswered(const quint64 generation, const TokenSource::Token& token) {
    if (generation != m_generation || m_state != State::Token)
        return;
    switch (token.outcome) {
    case TokenSource::Outcome::Ok:
        if (!token.value.isEmpty()) {
            m_state = State::Connecting;
            m_clock.arm(PeerTimer::RelayReady, relayReadyTimeoutMs);
            m_socket.open(token.value);
            return;
        }
        break;
    case TokenSource::Outcome::Retry:
        break;
    case TokenSource::Outcome::InvalidAccess:
        qCWarning(lcRunnerPeer) << "relay socket stopped: the runner credential was refused";
        halt();
        m_listener.tokenRejected();
        return;
    case TokenSource::Outcome::RunnerRemoved:
        qCWarning(lcRunnerPeer) << "relay socket stopped: the runner was removed";
        halt();
        m_listener.runnerRemoved();
        return;
    }
    retryOnBackoff(false);
}

void RelayLink::retryAfter(const qint64 delayMs, const bool renew) {
    m_state = State::Waiting;
    m_renewNext = renew;
    m_clock.arm(PeerTimer::RelayRetry, delayMs);
}

void RelayLink::retryOnBackoff(const bool renew) {
    ++m_failures;
    retryAfter(m_backoff.next(), renew);
}

void RelayLink::timerFired(const PeerTimer timer) {
    if (timer == PeerTimer::RelayRetry && m_state == State::Waiting) {
        connectNow(m_renewNext);
    } else if (timer == PeerTimer::RelayReady && m_state == State::Connecting) {
        qCInfo(lcRunnerPeer) << "relay socket gave no ready in time";
        m_socket.close();
        m_last401 = false;
        retryOnBackoff(false);
    }
}

void RelayLink::socketOpened() {}

void RelayLink::socketText(const QString& text) {
    if (m_state != State::Connecting && m_state != State::Ready)
        return;
    const QJsonObject message = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = message.value(QStringLiteral("type")).toString();
    if (m_state == State::Connecting) {
        if (type != QLatin1StringView("ready"))
            return;
        m_clock.disarm(PeerTimer::RelayReady);
        if (message.value(QStringLiteral("role")).toString() != QLatin1StringView("runner")) {
            qCWarning(lcRunnerPeer) << "relay socket ready with another role";
            m_socket.close();
            m_last401 = false;
            retryOnBackoff(false);
            return;
        }
        m_state = State::Ready;
        m_readyAt = m_clock.now().toMSecsSinceEpoch();
        m_failures = 0;
        m_last401 = false;
        qCInfo(lcRunnerPeer) << "relay socket ready";
        m_listener.ready();
        return;
    }
    if (type.startsWith(QLatin1StringView("signal.")))
        m_listener.signal(message);
    else if (dispatchWakeup(message, m_wakeups) == WakeupResult::Malformed)
        qCInfo(lcRunnerPeer) << "relay wake-up dropped: malformed" << type;
}

void RelayLink::socketClosed(const int code) {
    if (m_state != State::Connecting && m_state != State::Ready)
        return;
    const bool wasReady = m_state == State::Ready;
    const bool healthy =
        wasReady && m_clock.now().toMSecsSinceEpoch() - m_readyAt >= relayHealthyMs;
    m_clock.disarm(PeerTimer::RelayReady);
    m_last401 = false;
    if (healthy)
        m_backoff.reset();
    m_state = State::Waiting;
    qCInfo(lcRunnerPeer) << "relay socket closed with code" << code;
    if (wasReady) {
        const quint64 generation = m_generation;
        m_listener.lost();
        if (generation != m_generation || m_state != State::Waiting)
            return; // the listener stopped or restarted the link
    }
    if (code == closeRunnerRemoved) {
        halt();
        m_listener.runnerRemoved();
    } else if (code == closeCredentialExpired && healthy) {
        ++m_failures;
        connectNow(true);
    } else {
        retryOnBackoff(code == closeCredentialExpired);
    }
}

void RelayLink::socketRefused(const int status) {
    if (m_state != State::Connecting)
        return;
    m_clock.disarm(PeerTimer::RelayReady);
    m_state = State::Waiting;
    qCInfo(lcRunnerPeer) << "relay socket refused with status" << status;
    const quint64 generation = m_generation;
    m_listener.refused(status);
    if (generation != m_generation || m_state != State::Waiting)
        return;
    if (status == statusUnauthorized && !m_last401) {
        m_last401 = true;
        ++m_failures;
        connectNow(true);
        return;
    }
    const bool firstFailure = m_failures == 0;
    if (status != statusUnauthorized)
        m_last401 = false;
    retryOnBackoff(status == 0 && firstFailure);
}

} // namespace ketplus::runner::peer
