#include "run/ClientRelayPort.h"

#include "Environment.h"

namespace ketplus::runner::app {

AccessTokens::Call ClientRelayPort::heartbeat(const QJsonObject& body) {
    return m_context.tokens.call(
        [&](const QString& token) { return m_context.relay.heartbeat(body, token); });
}

delivery::CallResult ClientRelayPort::lease(const delivery::LeaseCall& call) {
    return m_leases.call(call);
}

// The token the Relay socket opens with: the one in use, renewed first when asked.
RelayPort::Token ClientRelayPort::token(const bool renew) {
    if (renew) {
        const AccessTokens::Result renewed = m_context.tokens.renew();
        if (renewed.outcome != AccessTokens::Outcome::Ok)
            return {renewed.outcome, {}};
    }
    Token token;
    const AccessTokens::Call call = m_context.tokens.callOnce([&](const QString& value) {
        token.value = value;
        RelayResult held;
        held.kind = RelayResult::Kind::Ok;
        return held;
    });
    token.outcome = call.token.outcome;
    if (token.outcome != AccessTokens::Outcome::Ok)
        token.value.clear();
    return token;
}

// Checks every pending pin once. Returns an exit code when one of them ends the run.
std::optional<ExitCode> ClientRelayPort::pairings() {
    PairingContext& context = m_context;
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

} // namespace ketplus::runner::app
