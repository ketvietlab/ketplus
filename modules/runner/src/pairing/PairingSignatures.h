#pragma once

#include "pairing/PairingResult.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <optional>

// Canonical signed payloads of runner pairing and peer authentication, with Ed25519 signing
// and verification. Each payload's bytes() is
// canonical(["ketplus", 2, <purpose>, ...fields]) in the order of the runner protocol.
// Verification checks, in order: the key, the key version, issuedAt against the caller's
// clock, then the signature.
namespace ketplus::runner::pairing {

// issuedAt may differ from the verifier's clock by at most this much either way.
inline constexpr qint64 issuedAtWindowMs = 5 * 60 * 1000;

// `now` as signed payloads carry it: RFC 3339 UTC with milliseconds.
[[nodiscard]] QString issuedAtString(const QDateTime& now);
// Lowercase hex SHA-256 of the exact UTF-8 SDP.
[[nodiscard]] QString sdpDigest(const QString& sdp);

struct VerifyingKey {
    QByteArray publicKey; // 32-byte Ed25519 public key
    int keyVersion = 0;
};

// runner.register and runner.pairing.claim, signed by the runner key.
struct RegisterPayload {
    QString ticketId;
    QString workspaceId;
    QString machineId;
    int runnerKeyVersion = 0;
    QString runnerFingerprint; // of the signing key; checked against it on verification
    QString issuedAt;
    [[nodiscard]] QByteArray bytes() const;
};

// runner.pairing.approve, signed by the desktop's machine key. The bytes hold no time and no
// key version; a runner verifies an approval only against its pending pin (verifyApproval).
struct ApprovalPayload {
    QString ticketId;
    QString pairingDigest;
    [[nodiscard]] QByteArray bytes() const;
};

// runner.token challenge, signed by the runner key.
struct ChallengePayload {
    QString runnerId;
    int runnerKeyVersion = 0;
    QString nonce;
    QString issuedAt;
    [[nodiscard]] QByteArray bytes() const;
};

// Offer of a runner peer connection, signed by the runner key.
struct OfferPayload {
    QString connectionId;
    QString workspaceId;
    QString machineId;
    QString runnerId;
    int runnerKeyVersion = 0;
    QString jobId;
    int attempt = 0;
    QString issuedAt;
    QString sdp; // signed as sdpDigest(sdp)
    [[nodiscard]] QByteArray bytes() const;
};

// Desktop answer to an offer, signed by the machine key.
struct AnswerPayload {
    QString connectionId;
    QString workspaceId;
    QString machineId;
    int machineKeyVersion = 0;
    QString runnerId;
    QString jobId;
    QString offerSdp; // signed as sdpDigest(offerSdp)
    QString issuedAt;
    QString sdp; // signed as sdpDigest(sdp)
    [[nodiscard]] QByteArray bytes() const;
};

// A 64-byte signature of `bytes` with a 32-byte Ed25519 private key; empty on failure.
[[nodiscard]] QByteArray signPayload(const QByteArray& privateKey, const QByteArray& bytes);

[[nodiscard]] std::optional<PairingError> verify(const RegisterPayload& payload,
                                                 const QByteArray& signature,
                                                 const VerifyingKey& key, const QDateTime& now);
[[nodiscard]] std::optional<PairingError> verify(const ChallengePayload& payload,
                                                 const QByteArray& signature,
                                                 const VerifyingKey& key, const QDateTime& now);
[[nodiscard]] std::optional<PairingError> verify(const OfferPayload& payload,
                                                 const QByteArray& signature,
                                                 const VerifyingKey& key, const QDateTime& now);
[[nodiscard]] std::optional<PairingError> verify(const AnswerPayload& payload,
                                                 const QByteArray& signature,
                                                 const VerifyingKey& key, const QDateTime& now);

} // namespace ketplus::runner::pairing
