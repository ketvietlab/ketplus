#pragma once

#include "runner/jobs/RemoteJobPorts.h"

#include <QHash>
#include <QObject>
#include <QPair>

#include <functional>
#include <memory>

class QTimer;

// The coordinator's clock on the event loop: one single-shot timer per task and kind, held in
// memory. Arming an armed timer replaces its time; a disarmed timer never fires; a timer that
// fires is gone afterwards.
namespace ketplus::daemon::runner::jobs {

class QtJobClock final : public JobClock {
  public:
    using Fired = std::function<void(const QString& taskId, JobTimer timer)>;

    explicit QtJobClock(Fired fired = {});
    ~QtJobClock() override;
    QtJobClock(const QtJobClock&) = delete;
    QtJobClock& operator=(const QtJobClock&) = delete;

    // Where expiries go, such as RemoteJobCoordinator::timerFired.
    void setFired(Fired fired) { fired_ = std::move(fired); }

    [[nodiscard]] QDateTime now() const override;
    void arm(const QString& taskId, JobTimer timer, qint64 delayMs) override;
    void disarm(const QString& taskId, JobTimer timer) override;

    [[nodiscard]] bool armed(const QString& taskId, JobTimer timer) const;
    [[nodiscard]] int size() const { return timers_.size(); }

  private:
    using Key = QPair<QString, int>;

    Fired fired_;
    QHash<Key, QTimer*> timers_;
    std::unique_ptr<QObject> context_;
};

} // namespace ketplus::daemon::runner::jobs
