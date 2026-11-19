#pragma once

#include "runner/jobs/QtJobClock.h"

#include <QDateTime>
#include <QHash>
#include <QPair>

// The desktop's job clock on real timers, which the test can move forward: after skip(), now()
// is that much later and every armed timer fires when its due time comes on the moved clock.
// Only the desktop's job records see the skipped time; the runner and the peer keep real time.
namespace runner_desktop_test {

class SkippableJobClock final : public ketplus::daemon::runner::jobs::JobClock {
  public:
    using Fired = ketplus::daemon::runner::jobs::QtJobClock::Fired;
    using JobTimer = ketplus::daemon::runner::jobs::JobTimer;

    SkippableJobClock();

    void setFired(Fired fired) { m_fired = std::move(fired); }
    void skip(qint64 ms);

    [[nodiscard]] QDateTime now() const override;
    void arm(const QString& taskId, JobTimer timer, qint64 delayMs) override;
    void disarm(const QString& taskId, JobTimer timer) override;

  private:
    using Key = QPair<QString, int>;

    ketplus::daemon::runner::jobs::QtJobClock m_timers;
    Fired m_fired;
    qint64 m_offsetMs = 0;
    QHash<Key, qint64> m_due; // msecs since epoch on the moved clock
};

} // namespace runner_desktop_test
