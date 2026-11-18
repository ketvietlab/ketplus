#include "Environment.h"
#include "commands/CommandSupport.h"
#include "commands/Commands.h"
#include "flow/PairingFlow.h"
#include "state/RunnerState.h"

namespace ketplus::runner::app {

ExitCode pairDesktop(const CommandOptions& options, const Environment& environment) {
    if (!supportedPlatform(environment))
        return ExitCode::UnsupportedPlatform;
    const std::optional<pairing::PairingToken> token = readToken(options, environment);
    if (!token)
        return ExitCode::Usage;
    if (token->kind() != pairing::PairingToken::Kind::AddDesktop) {
        environment.err(QStringLiteral("This pairing token registers a new runner; use "
                                       "`ketplus-runner register`."));
        return ExitCode::Usage;
    }
    if (!relayConfigured(environment))
        return ExitCode::Failure;
    const std::optional<StateDirectory> directory = openState(options, environment);
    if (!directory)
        return ExitCode::State;
    const RunnerState state(*directory);
    if (!state.hasIdentity()) {
        environment.err(QStringLiteral("This runner is not registered; run `ketplus-runner "
                                       "register` with a token for a new runner first."));
        return ExitCode::Usage;
    }
    QString error;
    std::optional<RunnerIdentity> identity = state.loadIdentity(&error);
    if (!identity) {
        environment.err(error);
        return ExitCode::State;
    }
    const pairing::PairingTokenFields& fields = token->fields();
    if (fields.runnerId != identity->credential.runnerId ||
        fields.workspaceId != identity->credential.workspaceId) {
        environment.err(QStringLiteral("This pairing token was made for another runner or "
                                       "workspace."));
        return ExitCode::Usage;
    }

    RelayClient relay(environment.relayUrl, environment.requestTimeoutMs);
    AccessTokens tokens(relay, state, *identity, environment);
    PairingContext context{environment, relay, state, tokens};
    const RunnerKey key{identity->privateKey, identity->publicKey, identity->credential.keyVersion};
    const QJsonObject body = signedTicketFields(*token, key, environment.now());
    const auto claim = [&](const QString& accessToken) {
        return relay.claim(fields.ticketId, body, accessToken);
    };
    AccessTokens::Call call = tokens.call(claim);
    bool retried = false;
    if (call.token.outcome == AccessTokens::Outcome::Ok && call.relay.lostAnswer()) {
        retried = true; // the same body: same issuedAt and signature
        call = tokens.call(claim);
    }
    if (call.token.outcome != AccessTokens::Outcome::Ok)
        return reportTokenFailure(call.token, environment);
    RelayResult& result = call.relay;
    const std::optional<pairing::ReportedDesktop> desktop = reportedDesktop(result.body);
    if (result.ok() && !desktop)
        result.kind = RelayResult::Kind::BadReply;
    if (!result.ok()) {
        if (result.kind != RelayResult::Kind::Refused ||
            (retried && result.refused("ticket_used"))) {
            reportLostAnswer(environment);
            return ExitCode::Failure;
        }
        if (result.refused("already_paired")) {
            environment.err(QStringLiteral("This runner is already paired with desktop %1.")
                                .arg(fields.machineId));
            return ExitCode::Failure;
        }
        if (result.refused("runner_removed"))
            return reportTokenFailure({AccessTokens::Outcome::RunnerRemoved, {}, {}}, environment);
        return reportTicketRefusal(result, environment);
    }

    if (pairing::checkDesktopKeys(*token, *desktop)) {
        environment.err(QStringLiteral("The Relay reported a desktop the pairing token does not "
                                       "name (desktop_key_mismatch). Nothing was pinned and the "
                                       "pair is being revoked."));
        static_cast<void>(tokens.callOnce([&](const QString& accessToken) {
            return relay.revokePair(fields.machineId, accessToken);
        }));
        return ExitCode::DesktopKeyMismatch;
    }
    return pinAndWait(*token, *desktop, identity->credential.runnerId, key, context);
}

} // namespace ketplus::runner::app
