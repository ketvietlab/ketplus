#include "Environment.h"
#include "RunnerRules.h"
#include "cloud/CryptoPrimitives.h"
#include "commands/CommandSupport.h"
#include "commands/Commands.h"
#include "flow/PairingFlow.h"
#include "protocol/Capabilities.h"
#include "state/RunnerState.h"

#include <QJsonArray>

namespace ketplus::runner::app {
namespace {

// Removes what this register created. Its own failure is printed and otherwise ignored: the
// exit code already says what went wrong.
void discardIdentity(const RunnerState& state, const Environment& environment) {
    QString error;
    if (!state.removeIdentity(&error))
        environment.err(error);
}

bool isSecretText(const QString& text) { return text.size() >= 32 && text.size() <= 512; }

} // namespace

ExitCode registerRunner(const CommandOptions& options, const Environment& environment) {
    if (!supportedPlatform(environment))
        return ExitCode::UnsupportedPlatform;
    const std::optional<QString> name =
        normalizedName(options.name.value_or(defaultName(environment.hostName)));
    if (!name) {
        environment.err(QStringLiteral("The runner name must be 1 to 64 characters without "
                                       "control characters; pass --name."));
        return ExitCode::Usage;
    }
    if (const QString error = labelsError(options.labels); !error.isEmpty()) {
        environment.err(error);
        return ExitCode::Usage;
    }
    const std::optional<pairing::PairingToken> token = readToken(options, environment);
    if (!token)
        return ExitCode::Usage;
    if (token->kind() != pairing::PairingToken::Kind::NewRunner) {
        environment.err(QStringLiteral("This pairing token adds a desktop to a runner that is "
                                       "already registered; use `ketplus-runner pair`."));
        return ExitCode::Usage;
    }
    if (!relayConfigured(environment))
        return ExitCode::Failure;
    const std::optional<StateDirectory> directory = openState(options, environment);
    if (!directory)
        return ExitCode::State;
    const RunnerState state(*directory);
    if (state.hasIdentity()) {
        environment.err(QStringLiteral("%1 already holds a runner. Use `ketplus-runner run` or "
                                       "`ketplus-runner status`; to add a desktop, use "
                                       "`ketplus-runner pair`.")
                            .arg(directory->path()));
        return ExitCode::Usage;
    }

    RunnerIdentity identity;
    const control_crypto::Ed25519KeyPair keys = control_crypto::generateEd25519();
    identity.privateKey = keys.privateKey;
    identity.publicKey = keys.publicKey;
    identity.config = {*name, options.labels};
    QString error;
    if (keys.privateKey.size() != 32 || !state.saveKey(identity.privateKey, &error) ||
        !state.saveConfig(identity.config, &error)) {
        environment.err(error.isEmpty() ? QStringLiteral("Cannot create the runner key") : error);
        discardIdentity(state, environment);
        return ExitCode::State;
    }

    const RunnerKey key{identity.privateKey, identity.publicKey, 1};
    QJsonObject body = signedTicketFields(*token, key, environment.now());
    body.insert(QStringLiteral("ticketId"), token->fields().ticketId);
    body.insert(QStringLiteral("publicKey"),
                QString::fromLatin1(control_crypto::base64Url(identity.publicKey)));
    body.insert(QStringLiteral("keyVersion"), key.keyVersion);
    body.insert(QStringLiteral("name"), *name);
    body.insert(QStringLiteral("os"), environment.platform.os);
    body.insert(QStringLiteral("arch"), environment.platform.arch);
    body.insert(QStringLiteral("labels"), QJsonArray::fromStringList(options.labels));
    body.insert(QStringLiteral("capabilities"),
                QJsonArray{QString(protocol::capability::runnerProtocol)});
    body.insert(QStringLiteral("runnerVersion"), environment.runnerVersion);

    RelayClient relay(environment.relayUrl, environment.requestTimeoutMs);
    RelayResult result = relay.registerRunner(body);
    bool retried = false;
    if (result.lostAnswer()) {
        retried = true; // the same body: same issuedAt and signature
        result = relay.registerRunner(body);
    }
    const QString runnerId = result.body.value(QStringLiteral("runnerId")).toString();
    const QString workspaceId = result.body.value(QStringLiteral("workspaceId")).toString();
    const QString refresh = result.body.value(QStringLiteral("refreshCredential")).toString();
    const std::optional<pairing::ReportedDesktop> desktop = reportedDesktop(result.body);
    if (result.ok() &&
        (!isRelayId(runnerId) || !isRelayId(workspaceId) || !isSecretText(refresh) || !desktop))
        result.kind = RelayResult::Kind::BadReply;
    if (!result.ok()) {
        discardIdentity(state, environment);
        if (result.kind != RelayResult::Kind::Refused ||
            (retried && result.refused("ticket_used"))) {
            reportLostAnswer(environment);
            return ExitCode::Failure;
        }
        return reportTicketRefusal(result, environment);
    }

    // The refresh credential is on disk before its first use.
    identity.credential = {runnerId, workspaceId, key.keyVersion, refresh};
    if (!state.saveCredential(identity.credential, &error)) {
        environment.err(error);
        return ExitCode::State;
    }
    AccessTokens tokens(relay, state, identity, environment);
    PairingContext context{environment, relay, state, tokens};

    const bool sameWorkspace = workspaceId == token->fields().workspaceId;
    const std::optional<pairing::PairingError> mismatch =
        pairing::checkDesktopKeys(*token, *desktop);
    if (mismatch || !sameWorkspace) {
        environment.err(QStringLiteral("The Relay reported a desktop the pairing token does not "
                                       "name (desktop_key_mismatch). Nothing was pinned and the "
                                       "new runner is being removed."));
        static_cast<void>(tokens.callOnce(
            [&](const QString& accessToken) { return relay.deregister(accessToken); }));
        discardIdentity(state, environment);
        return ExitCode::DesktopKeyMismatch;
    }
    environment.out(QStringLiteral("Registered runner %1 as \"%2\".").arg(runnerId, *name));
    return pinAndWait(*token, *desktop, runnerId, key, context);
}

} // namespace ketplus::runner::app
