#pragma once

// Exit codes of ketplus-runner. They are a stable interface: scripts and service managers
// read them, and `ketplus-runner --help` lists them.
namespace ketplus::runner::app {

enum class ExitCode : int {
    Ok = 0,
    Failure = 1,             // network retries exhausted, or a refusal without its own code
    Usage = 2,               // bad arguments, or a token, name or label refused locally
    State = 3,               // state directory: permissions, owner, corrupt file, locked
    DesktopKeyMismatch = 4,  // the Relay reported desktop keys the token does not name
    ApprovalSignature = 5,   // the desktop's approval does not verify
    TicketRefused = 6,       // pairing rejected, expired, revoked or unknown
    InvalidAccess = 7,       // the refresh credential is refused: register again
    RunnerRemoved = 8,       // the runner was removed from the Relay
    UnsupportedPlatform = 9, // not Linux on x86_64 or arm64
};

[[nodiscard]] constexpr int toInt(const ExitCode code) { return static_cast<int>(code); }

} // namespace ketplus::runner::app
