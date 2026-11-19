#include "SkippableJobClock.h"

#include <utility>

namespace runner_desktop_test {

SkippableJobClock::SkippableJobClock() {
    m_timers.setFired([this](const QString& taskId, const JobTimer timer) {
        m_due.remove({taskId, static_cast<int>(timer)});
        if (m_fired)
            m_fired(taskId, timer);
    });
}

void SkippableJobClock::skip(const qint64 ms) {
    m_offsetMs += ms;
    const qint64 at = now().toMSecsSinceEpoch();
    const QHash<Key, qint64> due = m_due;
    for (auto it = due.cbegin(); it != due.cend(); ++it)
        m_timers.arm(it.key().first, static_cast<JobTimer>(it.key().second),
                     qMax<qint64>(0, it.value() - at));
}

QDateTime SkippableJobClock::now() const { return m_timers.now().addMSecs(m_offsetMs); }

void SkippableJobClock::arm(const QString& taskId, const JobTimer timer, const qint64 delayMs) {
    const qint64 delay = qMax<qint64>(0, delayMs);
    m_due.insert({taskId, static_cast<int>(timer)}, now().toMSecsSinceEpoch() + delay);
    m_timers.arm(taskId, timer, delay);
}

void SkippableJobClock::disarm(const QString& taskId, const JobTimer timer) {
    m_due.remove({taskId, static_cast<int>(timer)});
    m_timers.disarm(taskId, timer);
}

} // namespace runner_desktop_test
