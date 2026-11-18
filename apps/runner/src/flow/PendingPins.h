#pragma once

#include "ExitCode.h"
#include "pairing/PairingPins.h"
#include "relay/RelayClient.h"
#include "run/AccessTokens.h"
#include "state/RunnerState.h"

#include <QString>

// A pending pin becomes active only once the desktop's approval verifies with the pinned
// desktop key over the digest the runner computed itself. Used by `register` and `pair`
// while they wait, and by `run` for pins still pending.
namespace ketplus::runner::app {

struct Environment;

struct PairingContext {
    const Environment& environment;
    RelayClient& relay;
    const RunnerState& state;
    AccessTokens& tokens;
};

enum class PendingOutcome {
    Active,      // verified and stored as active
    Waiting,     // not decided yet
    Refused,     // rejected, expired, revoked or unknown: the pending pin is removed
    BadApproval, // the approval does not verify: pin removed, pair revoked once
    InvalidAccess,
    RunnerRemoved,
    StateFailure,
    Retry, // network failure or an unexpected answer
};

struct PendingCheck {
    PendingOutcome outcome = PendingOutcome::Retry;
    QString detail; // the pair state or the refusal; never a secret
};

// One runner.pairing.state call for `pin` and what follows from its answer.
[[nodiscard]] PendingCheck checkPendingPin(const pairing::RunnerPin& pin, PairingContext& context);

// Polls every 2 seconds (with backoff on failures) until the pin is decided or a stop is
// requested, printing the outcome.
[[nodiscard]] ExitCode waitForApproval(const pairing::RunnerPin& pin, PairingContext& context);

// Prints why an access token could not be had and returns the exit code that goes with it.
[[nodiscard]] ExitCode reportTokenFailure(const AccessTokens::Result& result,
                                          const Environment& environment);

} // namespace ketplus::runner::app
