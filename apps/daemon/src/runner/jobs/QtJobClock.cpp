#include "runner/jobs/QtJobClock.h"

#include <QTimer>

#include <limits>
#include <utility>

namespace ketplus::daemon::runner::jobs {

namespace {

// QTimer takes an int of milliseconds; longer delays are re-armed for the rest when they fire.
constexpr qint64 longestStep = std::numeric_limits<int>::max();

} // namespace

QtJobClock::QtJobClock(Fired fired)
    : fired_(std::move(fired)), context_(std::make_unique<QObject>()) {}

QtJobClock::~QtJobClock() {
    for (QTimer* timer : std::as_const(timers_))
        delete timer;
}

QDateTime QtJobClock::now() const { return QDateTime::currentDateTimeUtc(); }

void QtJobClock::arm(const QString& taskId, const JobTimer timer, const qint64 delayMs) {
    disarm(taskId, timer);
    const Key key{taskId, static_cast<int>(timer)};
    auto* single = new QTimer(context_.get());
    single->setSingleShot(true);
    single->setTimerType(Qt::PreciseTimer);
    const qint64 delay = qMax<qint64>(0, delayMs);
    const qint64 step = qMin(delay, longestStep);
    const qint64 rest = delay - step;
    timers_.insert(key, single);
    QObject::connect(single, &QTimer::timeout, context_.get(), [this, key, single, rest] {
        if (timers_.value(key) != single)
            return;
        timers_.remove(key);
        single->deleteLater();
        const auto timer = static_cast<JobTimer>(key.second);
        if (rest > 0) {
            arm(key.first, timer, rest);
            return;
        }
        if (fired_)
            fired_(key.first, timer);
    });
    single->start(static_cast<int>(step));
}

void QtJobClock::disarm(const QString& taskId, const JobTimer timer) {
    QTimer* single = timers_.take({taskId, static_cast<int>(timer)});
    if (single == nullptr)
        return;
    single->stop();
    single->deleteLater();
}

bool QtJobClock::armed(const QString& taskId, const JobTimer timer) const {
    return timers_.contains({taskId, static_cast<int>(timer)});
}

} // namespace ketplus::daemon::runner::jobs
