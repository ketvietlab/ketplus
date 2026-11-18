#include "runner/jobs/PinRunnerTrust.h"

#include "runner/RunnerPinStore.h"

namespace ketplus::daemon::runner::jobs {

std::optional<QString> PinRunnerTrust::pinnedName(const QString& runnerId) const {
    if (runnerId.isEmpty())
        return std::nullopt;
    const auto pin = pins_.pin(runnerId);
    if (!pin || pin->runnerId != runnerId)
        return std::nullopt;
    return pin->name;
}

} // namespace ketplus::daemon::runner::jobs
