#include "peer/QtPeerClock.h"

#include <algorithm>
#include <limits>

namespace ketplus::runner::peer {

QtPeerClock::QtPeerClock(Fired fired) : m_fired(std::move(fired)) {
    for (std::size_t i = 0; i < timerCount; ++i) {
        QTimer& timer = m_timers.at(i);
        timer.setSingleShot(true);
        timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, &timer,
                         [this, i] { m_fired(static_cast<PeerTimer>(i)); });
    }
}

QDateTime QtPeerClock::now() const { return QDateTime::currentDateTimeUtc(); }

void QtPeerClock::arm(const PeerTimer timer, const qint64 delayMs) {
    const qint64 bounded = std::clamp<qint64>(delayMs, 0, std::numeric_limits<int>::max());
    m_timers.at(static_cast<std::size_t>(timer)).start(static_cast<int>(bounded));
}

void QtPeerClock::disarm(const PeerTimer timer) {
    m_timers.at(static_cast<std::size_t>(timer)).stop();
}

} // namespace ketplus::runner::peer
