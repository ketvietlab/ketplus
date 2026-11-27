#pragma once

#include "peer/PeerPorts.h"

#include <QTimer>

#include <array>
#include <functional>

// PeerClock over the system clock and single-shot QTimers.
namespace ketplus::runner::peer {

class QtPeerClock final : public PeerClock {
  public:
    using Fired = std::function<void(PeerTimer)>;
    explicit QtPeerClock(Fired fired);

    [[nodiscard]] QDateTime now() const override;
    void arm(PeerTimer timer, qint64 delayMs) override;
    void disarm(PeerTimer timer) override;

  private:
    static constexpr std::size_t timerCount = 4;
    Fired m_fired;
    std::array<QTimer, timerCount> m_timers;
};

} // namespace ketplus::runner::peer
