#pragma once

#include "pairing/PairingIdentity.h"
#include "pairing/PairingResult.h"
#include "pairing/PairingToken.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <optional>

// Pin records of a pair: the desktop a runner trusts, and the runner a desktop trusts. Only
// the types and their JSON form; storing them is the caller's. JSON carries keys in base64url
// without padding, times in RFC 3339 UTC and "version": 1. Reading checks every field and that
// the fingerprint matches the keys.
namespace ketplus::runner::pairing {

// Kept by a runner, one per desktop. Never holds the ticket secret or the token.
struct RunnerPin {
    enum class State {
        Pending, // stored at register or claim, before the desktop's approval verifies
        Active,
    };
    QString machineId;
    QString workspaceId;
    int machineKeyVersion = 0;
    QByteArray signingPublicKey;
    QByteArray agreementPublicKey;
    QString desktopFingerprint;
    State state = State::Pending;
    // Pending only: what the approval signature is checked against.
    QString ticketId;
    QString pairingDigest; // computed by the runner itself, never taken from the Relay
    // Active only; null while pending.
    std::optional<QDateTime> pairedAt;

    // A pending pin for the desktop the token names, once the reported keys match it
    // (checkDesktopKeys). `pairingDigest` is pairingCode(...).digest of the runner's own input.
    // A digest that is not base64url of 32 bytes fails with PinInvalid.
    [[nodiscard]] static PairingResult<RunnerPin> pending(const PairingToken& token,
                                                          const ReportedDesktop& desktop,
                                                          const QString& pairingDigest);
    // The same pin made active: ticket id and digest dropped, pairedAt set.
    [[nodiscard]] RunnerPin activated(const QDateTime& pairedAt) const;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PairingResult<RunnerPin> fromJson(const QJsonObject& object);
};

// Verifies a desktop's approval against a pending pin: the signature over the pin's own ticket
// id and pairing digest, with the pin's signing key. No time window applies. A key version
// reported alongside the approval must equal the pin's (KeyVersionMismatch); a pin that is not
// pending fails with PinInvalid.
[[nodiscard]] std::optional<PairingError>
verifyApproval(const RunnerPin& pin, const QByteArray& signature,
               std::optional<int> reportedMachineKeyVersion = std::nullopt);

// Kept by a desktop, one per runner.
struct DesktopRunnerPin {
    QString runnerId;
    int runnerKeyVersion = 0;
    QByteArray publicKey;
    QString runnerFingerprint;
    QString name;
    QDateTime pairedAt;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PairingResult<DesktopRunnerPin> fromJson(const QJsonObject& object);
};

} // namespace ketplus::runner::pairing
