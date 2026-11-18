#include "flow/PendingPins.h"

#include "Environment.h"
#include "pairing/PairingIdentity.h"
#include "run/Backoff.h"

namespace ketplus::runner::app {
namespace {

constexpr qint64 pollIntervalMs = 2'000;

PendingOutcome tokenOutcome(const AccessTokens::Outcome outcome) {
    switch (outcome) {
    case AccessTokens::Outcome::InvalidAccess:
        return PendingOutcome::InvalidAccess;
    case AccessTokens::Outcome::RunnerRemoved:
        return PendingOutcome::RunnerRemoved;
    case AccessTokens::Outcome::StateFailure:
        return PendingOutcome::StateFailure;
    case AccessTokens::Outcome::Ok:
    case AccessTokens::Outcome::Retry:
        break;
    }
    return PendingOutcome::Retry;
}

PendingCheck removed(const pairing::RunnerPin& pin, PairingContext& context,
                     const PendingOutcome outcome, const QString& detail) {
    QString error;
    if (!context.state.removePin(pin.machineId, &error)) {
        context.environment.err(error);
        return {PendingOutcome::StateFailure, detail};
    }
    return {outcome, detail};
}

} // namespace

PendingCheck checkPendingPin(const pairing::RunnerPin& pin, PairingContext& context) {
    const AccessTokens::Call call = context.tokens.call(
        [&](const QString& token) { return context.relay.pairingState(pin.ticketId, token); });
    if (call.token.outcome != AccessTokens::Outcome::Ok)
        return {tokenOutcome(call.token.outcome), call.token.relay.describe()};
    const RelayResult& answer = call.relay;
    if (answer.refused("ticket_not_found"))
        return removed(pin, context, PendingOutcome::Refused, answer.code);
    if (answer.refused("runner_removed"))
        return {PendingOutcome::RunnerRemoved, answer.code};
    if (answer.refused("invalid_access"))
        return {PendingOutcome::Retry, answer.describe()}; // a fresh token was just refused
    if (!answer.ok())
        return {PendingOutcome::Retry, answer.describe()};

    const QString state = answer.body.value(QStringLiteral("pairState")).toString();
    if (answer.body.value(QStringLiteral("ticketId")).toString() != pin.ticketId)
        return {PendingOutcome::Retry, QStringLiteral("unexpected reply")};
    if (state == QStringLiteral("ticketed") || state == QStringLiteral("claimed"))
        return {PendingOutcome::Waiting, state};
    if (state == QStringLiteral("rejected") || state == QStringLiteral("expired") ||
        state == QStringLiteral("revoked"))
        return removed(pin, context, PendingOutcome::Refused, state);
    if (state != QStringLiteral("active"))
        return {PendingOutcome::Retry, QStringLiteral("unexpected reply")};

    const std::optional<QByteArray> signature =
        pairing::decodeBase64Url(answer.body.value(QStringLiteral("approvalSignature")).toString());
    if (!signature || pairing::verifyApproval(pin, *signature)) {
        const PendingCheck check =
            removed(pin, context, PendingOutcome::BadApproval, QStringLiteral("active"));
        // One attempt to end the pair the runner will not trust; its result changes nothing.
        static_cast<void>(context.tokens.callOnce(
            [&](const QString& token) { return context.relay.revokePair(pin.machineId, token); }));
        return check;
    }
    QString error;
    if (!context.state.savePin(pin.activated(context.environment.now()), &error)) {
        context.environment.err(error);
        return {PendingOutcome::StateFailure, state};
    }
    return {PendingOutcome::Active, state};
}

ExitCode waitForApproval(const pairing::RunnerPin& pin, PairingContext& context) {
    const Environment& environment = context.environment;
    Backoff backoff(environment.random);
    while (true) {
        if (environment.stopRequested()) {
            environment.err(QStringLiteral("Stopped waiting. The pairing stays pending; "
                                           "`ketplus-runner run` keeps checking it."));
            return ExitCode::Failure;
        }
        const PendingCheck check = checkPendingPin(pin, context);
        switch (check.outcome) {
        case PendingOutcome::Active:
            environment.out(QStringLiteral("Paired with desktop %1.").arg(pin.machineId));
            return ExitCode::Ok;
        case PendingOutcome::Waiting:
            backoff.reset();
            environment.sleep(pollIntervalMs);
            continue;
        case PendingOutcome::Retry:
            environment.sleep(backoff.next());
            continue;
        case PendingOutcome::Refused:
            environment.err(QStringLiteral("The pairing with desktop %1 ended (%2). Create a new "
                                           "pairing token on the desktop and try again.")
                                .arg(pin.machineId, check.detail));
            return ExitCode::TicketRefused;
        case PendingOutcome::BadApproval:
            environment.err(QStringLiteral("The approval from desktop %1 does not verify with "
                                           "its pinned key. The pairing was not trusted.")
                                .arg(pin.machineId));
            return ExitCode::ApprovalSignature;
        case PendingOutcome::InvalidAccess:
            return reportTokenFailure({AccessTokens::Outcome::InvalidAccess, {}, {}}, environment);
        case PendingOutcome::RunnerRemoved:
            return reportTokenFailure({AccessTokens::Outcome::RunnerRemoved, {}, {}}, environment);
        case PendingOutcome::StateFailure:
            return ExitCode::State;
        }
    }
}

ExitCode reportTokenFailure(const AccessTokens::Result& result, const Environment& environment) {
    switch (result.outcome) {
    case AccessTokens::Outcome::Ok:
        return ExitCode::Ok;
    case AccessTokens::Outcome::InvalidAccess:
        environment.err(QStringLiteral(
            "The Relay no longer accepts this runner's credential. Register it again with a new "
            "pairing token: `ketplus-runner deregister --local-only`, then `ketplus-runner "
            "register`."));
        return ExitCode::InvalidAccess;
    case AccessTokens::Outcome::RunnerRemoved:
        environment.err(QStringLiteral("This runner was removed from the Relay. Register it "
                                       "again with a new pairing token."));
        return ExitCode::RunnerRemoved;
    case AccessTokens::Outcome::StateFailure:
        environment.err(result.error);
        return ExitCode::State;
    case AccessTokens::Outcome::Retry:
        break;
    }
    environment.err(
        QStringLiteral("Could not get an access token: %1").arg(result.relay.describe()));
    return ExitCode::Failure;
}

} // namespace ketplus::runner::app
