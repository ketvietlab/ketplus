#include "flow/PairingFlow.h"

#include "Environment.h"
#include "RunnerRules.h"
#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"

namespace ketplus::runner::app {

QJsonObject signedTicketFields(const pairing::PairingToken& token, const RunnerKey& key,
                               const QDateTime& now) {
    const pairing::PairingTokenFields& fields = token.fields();
    pairing::RegisterPayload payload;
    payload.ticketId = fields.ticketId;
    payload.workspaceId = fields.workspaceId;
    payload.machineId = fields.machineId;
    payload.runnerKeyVersion = key.keyVersion;
    payload.runnerFingerprint = pairing::runnerFingerprint(key.publicKey);
    payload.issuedAt = pairing::issuedAtString(now);
    const QByteArray signature = pairing::signPayload(key.privateKey, payload.bytes());
    return {
        {QStringLiteral("ticketSecret"), fields.ticketSecret},
        {QStringLiteral("issuedAt"), payload.issuedAt},
        {QStringLiteral("signature"), QString::fromLatin1(control_crypto::base64Url(signature))},
    };
}

std::optional<pairing::ReportedDesktop> reportedDesktop(const QJsonObject& reply) {
    const QJsonObject desktop = reply.value(QStringLiteral("desktop")).toObject();
    pairing::ReportedDesktop reported;
    reported.machineId = desktop.value(QStringLiteral("machineId")).toString();
    reported.machineKeyVersion =
        static_cast<int>(desktop.value(QStringLiteral("machineKeyVersion")).toInteger(0));
    const std::optional<QByteArray> signing =
        pairing::decodeBase64Url(desktop.value(QStringLiteral("signingPublicKey")).toString());
    const std::optional<QByteArray> agreement =
        pairing::decodeBase64Url(desktop.value(QStringLiteral("agreementPublicKey")).toString());
    if (!isRelayId(reported.machineId) || reported.machineKeyVersion < 1 || !signing ||
        !agreement || signing->size() != pairing::publicKeyBytes ||
        agreement->size() != pairing::publicKeyBytes ||
        reply.value(QStringLiteral("pairState")).toString() != QStringLiteral("claimed")) {
        return std::nullopt;
    }
    reported.signingPublicKey = *signing;
    reported.agreementPublicKey = *agreement;
    return reported;
}

ExitCode pinAndWait(const pairing::PairingToken& token, const pairing::ReportedDesktop& desktop,
                    const QString& runnerId, const RunnerKey& key, PairingContext& context) {
    const pairing::PairingTokenFields& fields = token.fields();
    const pairing::PairingCode code = pairing::pairingCode({
        fields.ticketId,
        fields.workspaceId,
        fields.machineId,
        fields.machineKeyVersion,
        fields.desktopFingerprint,
        runnerId,
        key.keyVersion,
        pairing::runnerFingerprint(key.publicKey),
    });
    const pairing::PairingResult<pairing::RunnerPin> pin =
        pairing::RunnerPin::pending(token, desktop, code.digest);
    QString error;
    if (!pin) {
        context.environment.err(QStringLiteral("The desktop's keys could not be pinned (%1)")
                                    .arg(pairing::errorCode(pin.error())));
        return ExitCode::Failure;
    }
    if (!context.state.savePin(pin.value(), &error)) {
        context.environment.err(error);
        return ExitCode::State;
    }
    context.environment.out(
        QStringLiteral("Confirmation code: %1\nCheck that the desktop shows the same code, then "
                       "approve the runner there. Waiting for approval…")
            .arg(code.code));
    return waitForApproval(pin.value(), context);
}

void reportLostAnswer(const Environment& environment) {
    environment.err(QStringLiteral(
        "The Relay did not answer, so it is unknown whether it accepted the pairing token. On "
        "the desktop, reject the waiting pairing request and create a new pairing token; also "
        "remove a runner with this name there if one appeared. Then try again."));
}

ExitCode reportTicketRefusal(const RelayResult& result, const Environment& environment) {
    if (result.refused("ticket_not_found") || result.refused("ticket_expired") ||
        result.refused("ticket_used")) {
        environment.err(QStringLiteral("The pairing token is no longer valid (%1). Create a new "
                                       "one on the desktop.")
                            .arg(result.code));
        return ExitCode::TicketRefused;
    }
    if (result.refused("invalid_labels")) {
        environment.err(QStringLiteral("The Relay refused the labels (invalid_labels)."));
        return ExitCode::Usage;
    }
    environment.err(
        QStringLiteral("The Relay did not accept the pairing token: %1").arg(result.describe()));
    return ExitCode::Failure;
}

} // namespace ketplus::runner::app
