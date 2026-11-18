#include "run/RunClocks.h"

#include <algorithm>

namespace ketplus::runner::app {

TimerKey timerKey(const session::SessionTimer timer) {
    return {TimerDomain::Session, static_cast<int>(timer)};
}

TimerKey timerKey(const delivery::DeliveryTimer timer) {
    return {TimerDomain::Delivery, static_cast<int>(timer)};
}

TimerKey timerKey(const peer::PeerTimer timer) {
    return {TimerDomain::Peer, static_cast<int>(timer)};
}

void HubSessionClock::arm(const session::SessionTimer timer, const qint64 delayMs) {
    m_hub.arm(timerKey(timer), delayMs);
}

void HubSessionClock::disarm(const session::SessionTimer timer) { m_hub.disarm(timerKey(timer)); }

void HubDeliveryClock::arm(const delivery::DeliveryTimer timer, const qint64 delayMs) {
    m_hub.arm(timerKey(timer), delayMs);
}

void HubDeliveryClock::disarm(const delivery::DeliveryTimer timer) {
    m_hub.disarm(timerKey(timer));
}

void HubPeerClock::arm(const peer::PeerTimer timer, const qint64 delayMs) {
    m_hub.arm(timerKey(timer), delayMs);
}

void HubPeerClock::disarm(const peer::PeerTimer timer) { m_hub.disarm(timerKey(timer)); }

void QtTimerHub::arm(const TimerKey key, const qint64 delayMs) {
    std::shared_ptr<QTimer>& timer = m_timers[code(key)];
    if (!timer) {
        timer = std::make_shared<QTimer>();
        timer->setSingleShot(true);
        timer->setTimerType(Qt::CoarseTimer);
        QObject::connect(timer.get(), &QTimer::timeout, timer.get(), [this, key] {
            if (m_fired)
                m_fired(key);
        });
    }
    // QTimer takes an int: the longest timer of the run (60 minutes) fits.
    timer->start(static_cast<int>(std::clamp<qint64>(delayMs, 0, 24LL * 60 * 60 * 1000)));
}

void QtTimerHub::disarm(const TimerKey key) {
    if (const auto found = m_timers.constFind(code(key)); found != m_timers.constEnd())
        found.value()->stop();
}

} // namespace ketplus::runner::app
