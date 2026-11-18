#pragma once

#include "pairing/PairingResult.h"

#include <QByteArray>
#include <QString>

#include <optional>

// Fingerprints, the pairing digest and its confirmation code, and the check of the desktop
// keys the Relay reports against the pairing token. Pure functions; keys are raw bytes.
namespace ketplus::runner::pairing {

class PairingToken;

// Bytes of an Ed25519 or X25519 public key.
inline constexpr qsizetype publicKeyBytes = 32;

// base64url(SHA-256(runner signing public key)); empty unless the key is 32 bytes.
[[nodiscard]] QString runnerFingerprint(const QByteArray& runnerPublicKey);
// base64url(SHA-256(signing public key || agreement public key)); empty unless both are 32
// bytes.
[[nodiscard]] QString desktopFingerprint(const QByteArray& signingPublicKey,
                                         const QByteArray& agreementPublicKey);

// Equality whose time depends only on the lengths, for digests, fingerprints and secrets.
[[nodiscard]] bool constantTimeEquals(const QByteArray& left, const QByteArray& right);

// Strict base64url without padding: any other character, padding or a non-canonical last
// character is refused.
[[nodiscard]] std::optional<QByteArray> decodeBase64Url(const QString& text);

struct PairingDigestInput {
    QString ticketId;
    QString workspaceId;
    QString machineId;
    int machineKeyVersion = 0;
    QString desktopFingerprint;
    QString runnerId;
    int runnerKeyVersion = 0;
    QString runnerFingerprint;
};

struct PairingCode {
    QString digest; // base64url of the SHA-256 of the canonical input
    QString code;   // first ten base32 characters of the same bytes, as XXXXX-XXXXX
};

[[nodiscard]] QByteArray pairingDigestBytes(const PairingDigestInput& input); // canonical input
[[nodiscard]] PairingCode pairingCode(const PairingDigestInput& input);

// The desktop as the Relay reports it in reply to a register or claim.
struct ReportedDesktop {
    QString machineId;
    int machineKeyVersion = 0;
    QByteArray signingPublicKey;
    QByteArray agreementPublicKey;
};

// DesktopKeyMismatch unless both keys hash to the token's desktop fingerprint and the machine
// id and key version equal the token's.
[[nodiscard]] std::optional<PairingError> checkDesktopKeys(const PairingToken& token,
                                                           const ReportedDesktop& desktop);

} // namespace ketplus::runner::pairing
