#pragma once

#include "runner/jobs/RemoteJobPorts.h"

namespace ketplus::daemon::runner {
class RunnerPinStore;
}

// Runner trust from this desktop's runner pins: a runner is trusted only while the pin store
// holds a pin for it that reads back.
namespace ketplus::daemon::runner::jobs {

class PinRunnerTrust final : public RunnerTrust {
  public:
    explicit PinRunnerTrust(const RunnerPinStore& pins) : pins_(pins) {}
    [[nodiscard]] std::optional<QString> pinnedName(const QString& runnerId) const override;

  private:
    const RunnerPinStore& pins_;
};

} // namespace ketplus::daemon::runner::jobs
