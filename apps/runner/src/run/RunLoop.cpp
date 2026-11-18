#include "run/RunLoop.h"

#include "Environment.h"
#include "RunnerRules.h"
#include "protocol/Capabilities.h"
#include "run/Backoff.h"

#include <QJsonArray>

namespace ketplus::runner::app {
namespace {

bool fatal(const AccessTokens::Outcome outcome) {
    return outcome == AccessTokens::Outcome::InvalidAccess ||
           outcome == AccessTokens::Outcome::RunnerRemoved ||
           outcome == AccessTokens::Outcome::StateFailure;
}

// Checks every pending pin once. Returns an exit code when one of them ends the run.
std::optional<ExitCode> finishPendingPairings(PairingContext& context) {
    const Environment& environment = context.environment;
    QString error;
    const std::optional<QList<pairing::RunnerPin>> pins = context.state.loadPins(&error);
    if (!pins) {
        environment.err(error);
        return ExitCode::State;
    }
    for (const pairing::RunnerPin& pin : *pins) {
        if (pin.state != pairing::RunnerPin::State::Pending)
            continue;
        const PendingCheck check = checkPendingPin(pin, context);
        switch (check.outcome) {
        case PendingOutcome::Active:
            environment.out(QStringLiteral("Paired with desktop %1.").arg(pin.machineId));
            break;
        case PendingOutcome::Refused:
            environment.err(QStringLiteral("The pairing with desktop %1 ended (%2).")
                                .arg(pin.machineId, check.detail));
            break;
        case PendingOutcome::BadApproval:
            environment.err(QStringLiteral("The approval from desktop %1 does not verify with "
                                           "its pinned key. The pairing was not trusted.")
                                .arg(pin.machineId));
            break;
        case PendingOutcome::InvalidAccess:
            return reportTokenFailure({AccessTokens::Outcome::InvalidAccess, {}, {}}, environment);
        case PendingOutcome::RunnerRemoved:
            return reportTokenFailure({AccessTokens::Outcome::RunnerRemoved, {}, {}}, environment);
        case PendingOutcome::StateFailure:
            return ExitCode::State;
        case PendingOutcome::Waiting:
        case PendingOutcome::Retry:
            break;
        }
    }
    return std::nullopt;
}

} // namespace

ExitCode runHeartbeatLoop(PairingContext& context, const RunnerIdentity& identity) {
    const Environment& environment = context.environment;
    const QJsonObject heartbeat{
        {QStringLiteral("state"), QStringLiteral("idle")},
        {QStringLiteral("labels"), QJsonArray::fromStringList(identity.config.labels)},
        {QStringLiteral("capabilities"), QJsonArray{QString(protocol::capability::runnerProtocol)}},
        {QStringLiteral("runnerVersion"), environment.runnerVersion},
        {QStringLiteral("currentJob"), QJsonValue(QJsonValue::Null)},
    };
    Backoff backoff(environment.random);
    bool disabled = false;
    environment.out(QStringLiteral("Runner %1 is running.").arg(identity.credential.runnerId));
    while (!environment.stopRequested()) {
        const AccessTokens::Call call = context.tokens.call(
            [&](const QString& token) { return context.relay.heartbeat(heartbeat, token); });
        if (fatal(call.token.outcome))
            return reportTokenFailure(call.token, environment);
        if (call.relay.refused("runner_removed"))
            return reportTokenFailure({AccessTokens::Outcome::RunnerRemoved, {}, {}}, environment);
        const QJsonValue reportedDisabled = call.relay.body.value(QStringLiteral("disabled"));
        if (call.token.outcome != AccessTokens::Outcome::Ok || !call.relay.ok() ||
            !reportedDisabled.isBool()) {
            environment.sleep(backoff.next());
            continue;
        }
        backoff.reset();
        if (reportedDisabled.toBool() != disabled) {
            disabled = reportedDisabled.toBool();
            environment.out(disabled ? QStringLiteral("This runner is disabled on the desktop; "
                                                      "it keeps reporting but takes no job.")
                                     : QStringLiteral("This runner is enabled again."));
        }
        if (const std::optional<ExitCode> code = finishPendingPairings(context))
            return *code;
        environment.sleep(heartbeatIntervalMs);
    }
    environment.out(QStringLiteral("Stopped."));
    return ExitCode::Ok;
}

} // namespace ketplus::runner::app
