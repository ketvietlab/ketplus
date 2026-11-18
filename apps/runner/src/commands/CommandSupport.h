#pragma once

#include "ExitCode.h"
#include "commands/Commands.h"
#include "pairing/PairingToken.h"
#include "state/StateDirectory.h"

#include <optional>

// Steps several commands begin with. Each prints its own error.
namespace ketplus::runner::app {

// Linux on x86_64 or arm64, as the Relay accepts.
[[nodiscard]] bool supportedPlatform(const Environment& environment);
[[nodiscard]] std::optional<StateDirectory> openState(const CommandOptions& options,
                                                      const Environment& environment);
// The token from the argument, stdin ("-") or --token-file, parsed. Nothing is echoed.
[[nodiscard]] std::optional<pairing::PairingToken> readToken(const CommandOptions& options,
                                                             const Environment& environment);
// False after printing that this build names no Relay.
[[nodiscard]] bool relayConfigured(const Environment& environment);

} // namespace ketplus::runner::app
