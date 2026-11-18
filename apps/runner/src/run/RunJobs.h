#pragma once

#include "ExitCode.h"
#include "flow/PendingPins.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

// `ketplus-runner run`: the production wiring of the run loop, until a stop signal or an exit.
namespace ketplus::runner::app {

[[nodiscard]] ExitCode runJobs(PairingContext& context, const RunnerIdentity& identity,
                               const StateDirectory& directory, const QString& workDirOption);

} // namespace ketplus::runner::app
