#include "run/RunnerLoop.h"

// Where the events of the pieces go. Each one that may change what the runner holds or
// reports ends in update().
namespace ketplus::runner::app {

using session::SessionTimer;

void RunnerLoop::fired(const TimerKey key) {
    if (m_done)
        return;
    switch (key.domain) {
    case TimerDomain::Run:
        switch (static_cast<RunTimer>(key.id)) {
        case RunTimer::Heartbeat:
            askHeartbeat();
            return;
        case RunTimer::CleanupBound:
            cleanupDone(false, true);
            return;
        case RunTimer::StopPoll:
            m_deps.timers.arm(runTimer(RunTimer::StopPoll), stopPollMs);
            checkStop();
            return;
        }
        return;
    case TimerDomain::Session:
        m_session.timerFired(static_cast<SessionTimer>(key.id));
        update();
        return;
    case TimerDomain::Delivery:
        m_delivery.timerFired(static_cast<delivery::DeliveryTimer>(key.id));
        update();
        return;
    case TimerDomain::Peer: {
        const auto timer = static_cast<peer::PeerTimer>(key.id);
        if (timer == peer::PeerTimer::RelayRetry || timer == peer::PeerTimer::RelayReady)
            m_link->timerFired(timer);
        else
            m_connector.timerFired(timer);
        update();
        return;
    }
    }
}

// The Relay socket.
void RunnerLoop::ready() {
    m_connector.relayReady();
    update();
}

void RunnerLoop::lost() { m_connector.relayLost(); } // a running job goes on

void RunnerLoop::refused(const int status) {
    m_deps.environment.err(QStringLiteral("The Relay refused the socket (%1).").arg(status));
}

void RunnerLoop::runnerRemoved() {
    m_connector.runnerRemoved();
    tokenFailure(AccessTokens::Outcome::RunnerRemoved);
}

void RunnerLoop::tokenRejected() { tokenFailure(AccessTokens::Outcome::InvalidAccess); }

void RunnerLoop::signal(const QJsonObject& message) {
    m_connector.signal(message);
    update();
}

bool RunnerLoop::relayReady() const { return m_link->isReady(); }

bool RunnerLoop::sendSignal(const QJsonObject& message) { return m_link->send(message); }

void RunnerLoop::socketOpened() { m_link->socketOpened(); }
void RunnerLoop::socketText(const QString& text) { m_link->socketText(text); }
void RunnerLoop::socketClosed(const int code) { m_link->socketClosed(code); }
void RunnerLoop::socketRefused(const int status) { m_link->socketRefused(status); }

// Wake-ups.
void RunnerLoop::jobAvailable(const QString& jobId) {
    if (!mayAcquire())
        return;
    m_delivery.jobAvailable(jobId);
    update();
}

// A cancel of the held lease is learned from its renewal, which goes now instead of later.
void RunnerLoop::jobCancel(const QString& jobId, const QString& leaseId) {
    const QJsonValue current = m_delivery.heartbeatFields().value(QStringLiteral("currentJob"));
    if (!current.isObject() ||
        current.toObject().value(QStringLiteral("jobId")).toString() != jobId ||
        current.toObject().value(QStringLiteral("leaseId")).toString() != leaseId)
        return;
    m_deps.timers.disarm(timerKey(delivery::DeliveryTimer::Renew));
    m_delivery.timerFired(delivery::DeliveryTimer::Renew);
    update();
}

void RunnerLoop::runnerPaired(const QString&, const QString&, const QString&) {
    m_queue.pairings();
}

void RunnerLoop::runnerUnpaired(const QString&) { m_queue.pairings(); }

// The peer connection.
void RunnerLoop::channelOpened(const QString& connectionId) {
    m_session.channelOpened(connectionId);
    update();
}

void RunnerLoop::channelLost(const QString&) {
    m_session.channelLost();
    update();
}

void RunnerLoop::frameReceived(const protocol::Channel channel, const QByteArray& message) {
    m_session.frameReceived(channel, message);
    update();
}

// A `bulk` transfer is named in its chunk frame by its 64-bit id as 16 lowercase hex digits.
void RunnerLoop::transferReceived(const quint64 transferId, const QByteArray& bytes) {
    m_session.bulkReceived(QStringLiteral("%1").arg(transferId, 16, 16, QLatin1Char('0')), bytes);
    update();
}

void RunnerLoop::eventsWritable() { m_session.eventsWritable(); }

// The session's own timers ask for the next connection; nothing more to do here.
void RunnerLoop::attemptFailed(const QString&) {}
void RunnerLoop::gaveUp(const QString&) {}

// The delivery's session, which also tells the connector whom to reach.
bool RunnerLoop::open(const delivery::HeldLease& lease) {
    m_channels.setRequest(peer::PeerRequest{lease.jobId, lease.leaseId,
                                            static_cast<int>(lease.attempt), lease.machineId});
    return m_sessionLink.open(lease);
}

bool RunnerLoop::withdraw() { return m_sessionLink.withdraw(); }

void RunnerLoop::leaseStarted(const QString& startedAt) { m_sessionLink.leaseStarted(startedAt); }

void RunnerLoop::leaseStartRefused(const QString& code) { m_sessionLink.leaseStartRefused(code); }

void RunnerLoop::leaseLost(const QString& code, const QString& reason) {
    m_sessionLink.leaseLost(code, reason);
}

QString RunnerLoop::phase() const { return m_sessionLink.phase(); }

} // namespace ketplus::runner::app
