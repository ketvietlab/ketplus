#pragma once

#include "runner/peer/PeerPorts.h"

#include <QHash>
#include <QObject>
#include <QPair>

#include <functional>

class QTimer;

namespace ketplus::daemon::runner::peer {

// PeerClock on Qt timers: one single-shot timer per (job, timer). The answer always comes from
// the event loop, never from inside arm(), and never after the clock is destroyed.
class QtJobPeerClock final : public PeerClock {
  public:
    using Fired = std::function<void(const QString& jobId, PeerTimer timer)>;

    explicit QtJobPeerClock(Fired fired);
    ~QtJobPeerClock() override;
    QtJobPeerClock(const QtJobPeerClock&) = delete;
    QtJobPeerClock& operator=(const QtJobPeerClock&) = delete;

    // The system's UTC time.
    [[nodiscard]] QDateTime now() const override;
    void arm(const QString& jobId, PeerTimer timer, qint64 delayMs) override;
    void disarm(const QString& jobId, PeerTimer timer) override;
    // Disarms every timer of the job.
    void disarmAll(const QString& jobId);
    [[nodiscard]] bool armed(const QString& jobId, PeerTimer timer) const;

  private:
    using Key = QPair<QString, int>;
    void release(QTimer* timer);

    Fired m_fired;
    QObject m_context;
    QHash<Key, QTimer*> m_timers;
};

} // namespace ketplus::daemon::runner::peer
