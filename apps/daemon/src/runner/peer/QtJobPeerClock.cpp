#include "runner/peer/QtJobPeerClock.h"

#include <QTimer>

#include <algorithm>
#include <limits>

namespace ketplus::daemon::runner::peer {

QtJobPeerClock::QtJobPeerClock(Fired fired) : m_fired(std::move(fired)) {}

QtJobPeerClock::~QtJobPeerClock() {
    for (QTimer* timer : std::as_const(m_timers))
        release(timer);
    m_timers.clear();
}

QDateTime QtJobPeerClock::now() const { return QDateTime::currentDateTimeUtc(); }

void QtJobPeerClock::arm(const QString& jobId, const PeerTimer timer, const qint64 delayMs) {
    const Key key{jobId, int(timer)};
    QTimer* qtimer = m_timers.value(key);
    if (!qtimer) {
        qtimer = new QTimer(&m_context);
        qtimer->setSingleShot(true);
        QObject::connect(qtimer, &QTimer::timeout, &m_context, [this, key, qtimer] {
            if (m_timers.value(key) != qtimer)
                return;
            m_timers.remove(key);
            release(qtimer);
            if (m_fired)
                m_fired(key.first, PeerTimer(key.second));
        });
        m_timers.insert(key, qtimer);
    }
    // Starting an active timer starts it again from now.
    qtimer->start(int(std::clamp<qint64>(delayMs, 0, std::numeric_limits<int>::max())));
}

void QtJobPeerClock::disarm(const QString& jobId, const PeerTimer timer) {
    if (QTimer* qtimer = m_timers.take({jobId, int(timer)}))
        release(qtimer);
}

void QtJobPeerClock::disarmAll(const QString& jobId) {
    for (auto it = m_timers.begin(); it != m_timers.end();) {
        if (it.key().first == jobId) {
            release(it.value());
            it = m_timers.erase(it);
        } else {
            ++it;
        }
    }
}

bool QtJobPeerClock::armed(const QString& jobId, const PeerTimer timer) const {
    return m_timers.contains({jobId, int(timer)});
}

void QtJobPeerClock::release(QTimer* timer) {
    // Out of the context first: a timer may be the one whose timeout is running.
    timer->stop();
    QObject::disconnect(timer, nullptr, &m_context, nullptr);
    timer->setParent(nullptr);
    timer->deleteLater();
}

} // namespace ketplus::daemon::runner::peer
