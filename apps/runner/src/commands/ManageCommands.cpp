#include "Environment.h"
#include "RunnerRules.h"
#include "commands/CommandSupport.h"
#include "commands/Commands.h"
#include "flow/PendingPins.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingSignatures.h"
#include "state/RunnerState.h"

namespace ketplus::runner::app {
namespace {

std::optional<RunnerIdentity> loadIdentity(const RunnerState& state, const Environment& environment,
                                           ExitCode* code) {
    if (!state.hasIdentity()) {
        environment.err(QStringLiteral("This runner is not registered."));
        *code = ExitCode::Usage;
        return std::nullopt;
    }
    QString error;
    std::optional<RunnerIdentity> identity = state.loadIdentity(&error);
    if (!identity) {
        environment.err(error);
        *code = ExitCode::State;
    }
    return identity;
}

ExitCode relayFailure(const AccessTokens::Call& call, const Environment& environment,
                      const QString& retry) {
    if (call.token.outcome != AccessTokens::Outcome::Ok) {
        const ExitCode code = reportTokenFailure(call.token, environment);
        environment.err(retry);
        return code;
    }
    environment.err(
        QStringLiteral("The Relay call failed: %1. %2").arg(call.relay.describe(), retry));
    return ExitCode::Failure;
}

} // namespace

ExitCode unpairDesktop(const CommandOptions& options, const Environment& environment) {
    if (options.positional.size() != 1 || !isRelayId(options.positional.front())) {
        environment.err(QStringLiteral("Usage: ketplus-runner unpair MACHINE_ID"));
        return ExitCode::Usage;
    }
    const QString machineId = options.positional.front();
    if (!relayConfigured(environment))
        return ExitCode::Failure;
    const std::optional<StateDirectory> directory = openState(options, environment);
    if (!directory)
        return ExitCode::State;
    const RunnerState state(*directory);
    ExitCode code = ExitCode::Ok;
    std::optional<RunnerIdentity> identity = loadIdentity(state, environment, &code);
    if (!identity)
        return code;

    // Trust ends first; the Relay is told afterwards, and a rerun finishes the job.
    QString error;
    if (!state.removePin(machineId, &error)) {
        environment.err(error);
        return ExitCode::State;
    }
    RelayClient relay(environment.relayUrl, environment.requestTimeoutMs);
    AccessTokens tokens(relay, state, *identity, environment);
    const AccessTokens::Call call =
        tokens.call([&](const QString& token) { return relay.revokePair(machineId, token); });
    if (call.token.outcome == AccessTokens::Outcome::Ok &&
        (call.relay.ok() || call.relay.refused("not_paired"))) {
        environment.out(QStringLiteral("Unpaired from desktop %1.").arg(machineId));
        return ExitCode::Ok;
    }
    return relayFailure(call, environment,
                        QStringLiteral("The desktop is no longer trusted here; run `ketplus-runner "
                                       "unpair %1` again to tell the Relay.")
                            .arg(machineId));
}

ExitCode deregisterRunner(const CommandOptions& options, const Environment& environment) {
    if (!options.positional.isEmpty()) {
        environment.err(QStringLiteral("Usage: ketplus-runner deregister [--local-only]"));
        return ExitCode::Usage;
    }
    if (!options.localOnly && !relayConfigured(environment))
        return ExitCode::Failure;
    const std::optional<StateDirectory> directory = openState(options, environment);
    if (!directory)
        return ExitCode::State;
    const RunnerState state(*directory);
    QString error;
    if (options.localOnly) {
        if (!state.removeIdentity(&error)) {
            environment.err(error);
            return ExitCode::State;
        }
        environment.out(QStringLiteral("Removed this runner's local state. Remove the runner on "
                                       "each desktop it was paired with."));
        return ExitCode::Ok;
    }
    ExitCode code = ExitCode::Ok;
    std::optional<RunnerIdentity> identity = loadIdentity(state, environment, &code);
    if (!identity)
        return code;
    RelayClient relay(environment.relayUrl, environment.requestTimeoutMs);
    AccessTokens tokens(relay, state, *identity, environment);
    const AccessTokens::Call call =
        tokens.call([&](const QString& token) { return relay.deregister(token); });
    const bool gone = call.token.outcome == AccessTokens::Outcome::RunnerRemoved ||
                      (call.token.outcome == AccessTokens::Outcome::Ok &&
                       (call.relay.ok() || call.relay.refused("runner_removed")));
    if (!gone) {
        return relayFailure(call, environment,
                            QStringLiteral("Nothing was removed. Run `ketplus-runner deregister` "
                                           "again, or `ketplus-runner deregister --local-only` "
                                           "when the Relay no longer accepts this runner."));
    }
    if (!state.removeIdentity(&error)) {
        environment.err(error);
        return ExitCode::State;
    }
    environment.out(QStringLiteral("Deregistered runner %1.").arg(identity->credential.runnerId));
    return ExitCode::Ok;
}

ExitCode showStatus(const CommandOptions& options, const Environment& environment) {
    if (!options.positional.isEmpty()) {
        environment.err(QStringLiteral("Usage: ketplus-runner status"));
        return ExitCode::Usage;
    }
    const std::optional<StateDirectory> directory = openState(options, environment);
    if (!directory)
        return ExitCode::State;
    const RunnerState state(*directory);
    environment.out(QStringLiteral("State directory: %1").arg(directory->path()));
    if (!state.hasIdentity()) {
        environment.out(QStringLiteral("Not registered."));
        return ExitCode::Ok;
    }
    QString error;
    const std::optional<RunnerIdentity> identity = state.loadIdentity(&error);
    const std::optional<QList<pairing::RunnerPin>> pins =
        identity ? state.loadPins(&error) : std::nullopt;
    if (!identity || !pins) {
        environment.err(error);
        return ExitCode::State;
    }
    environment.out(QStringLiteral("Runner: %1").arg(identity->credential.runnerId));
    environment.out(QStringLiteral("Workspace: %1").arg(identity->credential.workspaceId));
    environment.out(QStringLiteral("Name: %1").arg(identity->config.name));
    environment.out(QStringLiteral("Labels: %1")
                        .arg(identity->config.labels.isEmpty()
                                 ? QStringLiteral("(none)")
                                 : identity->config.labels.join(QStringLiteral(", "))));
    environment.out(
        QStringLiteral("Key fingerprint: %1").arg(pairing::runnerFingerprint(identity->publicKey)));
    if (pins->isEmpty())
        environment.out(QStringLiteral("Desktops: none"));
    for (const pairing::RunnerPin& pin : *pins) {
        environment.out(
            pin.state == pairing::RunnerPin::State::Active
                ? QStringLiteral("Desktop %1: paired since %2")
                      .arg(pin.machineId, pairing::issuedAtString(*pin.pairedAt))
                : QStringLiteral("Desktop %1: waiting for approval").arg(pin.machineId));
    }
    return ExitCode::Ok;
}

} // namespace ketplus::runner::app
