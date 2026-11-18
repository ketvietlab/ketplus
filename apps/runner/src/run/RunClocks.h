#pragma once

#include "delivery/DeliveryPorts.h"
#include "peer/PeerPorts.h"
#include "run/RunPorts.h"
#include "session/SessionPorts.h"

#include <QHash>
#include <QTimer>

#include <memory>

// The clocks of the session, the delivery and the peer pieces, each over the one TimerHub of
// the run; and the hub over Qt timers.
namespace ketplus::runner::app {

class HubSessionClock final : public session::SessionClock {
  public:
    explicit HubSessionClock(TimerHub& hub) : m_hub(hub) {}
    [[nodiscard]] QDateTime now() const override { return m_hub.now(); }
    void arm(session::SessionTimer timer, qint64 delayMs) override;
    void disarm(session::SessionTimer timer) override;

  private:
    TimerHub& m_hub;
};

class HubDeliveryClock final : public delivery::DeliveryClock {
  public:
    explicit HubDeliveryClock(TimerHub& hub) : m_hub(hub) {}
    void arm(delivery::DeliveryTimer timer, qint64 delayMs) override;
    void disarm(delivery::DeliveryTimer timer) override;

  private:
    TimerHub& m_hub;
};

class HubPeerClock final : public peer::PeerClock {
  public:
    explicit HubPeerClock(TimerHub& hub) : m_hub(hub) {}
    [[nodiscard]] QDateTime now() const override { return m_hub.now(); }
    void arm(peer::PeerTimer timer, qint64 delayMs) override;
    void disarm(peer::PeerTimer timer) override;

  private:
    TimerHub& m_hub;
};

[[nodiscard]] TimerKey timerKey(session::SessionTimer timer);
[[nodiscard]] TimerKey timerKey(delivery::DeliveryTimer timer);
[[nodiscard]] TimerKey timerKey(peer::PeerTimer timer);

// Single-shot Qt timers, created on first use. A timer fires from the event loop only.
class QtTimerHub final : public TimerHub {
  public:
    QtTimerHub() = default;
    QtTimerHub(const QtTimerHub&) = delete;
    QtTimerHub& operator=(const QtTimerHub&) = delete;

    [[nodiscard]] QDateTime now() const override { return QDateTime::currentDateTimeUtc(); }
    void arm(TimerKey key, qint64 delayMs) override;
    void disarm(TimerKey key) override;
    void onFired(Fired fired) override { m_fired = std::move(fired); }

  private:
    [[nodiscard]] static int code(TimerKey key) {
        return static_cast<int>(key.domain) * 64 + key.id;
    }

    Fired m_fired;
    QHash<int, std::shared_ptr<QTimer>> m_timers;
};

} // namespace ketplus::runner::app
