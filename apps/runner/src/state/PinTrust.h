#pragma once

#include "delivery/DeliveryPorts.h"

// A desktop is trusted when this runner holds an active pin for it, read from disk each time.
namespace ketplus::runner::app {

class RunnerState;

class PinTrust final : public delivery::RequesterTrust {
  public:
    explicit PinTrust(const RunnerState& state) : m_state(state) {}
    [[nodiscard]] bool trusted(const QString& machineId) const override;

  private:
    const RunnerState& m_state;
};

} // namespace ketplus::runner::app
