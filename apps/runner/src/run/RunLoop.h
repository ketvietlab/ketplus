#pragma once

#include "ExitCode.h"
#include "flow/PendingPins.h"

// `ketplus-runner run`: keeps an access token, sends a heartbeat every 30 seconds and finishes
// pairings still pending. It takes no job yet. Returns when a stop is requested (Ok) or the
// Relay refuses the runner for good.
namespace ketplus::runner::app {

inline constexpr qint64 heartbeatIntervalMs = 30'000;

[[nodiscard]] ExitCode runHeartbeatLoop(PairingContext& context, const RunnerIdentity& identity);

} // namespace ketplus::runner::app
