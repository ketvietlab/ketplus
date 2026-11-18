#include "pairing/PairingSignatures.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"

#include <QCryptographicHash>
#include <QJsonArray>

namespace ketplus::runner::pairing {
namespace cc = ketplus::control_crypto;
namespace {

QJsonArray header(const char* purpose) {
    return {QStringLiteral("ketplus"), 2, QString::fromLatin1(purpose)};
}

QByteArray canonicalOf(QJsonArray values, std::initializer_list<QJsonValue> fields) {
    for (const QJsonValue& field : fields)
        values.append(field);
    return cc::canonical(values);
}

std::optional<PairingError> checkIssuedAt(const QString& issuedAt, const QDateTime& now) {
    // RFC 3339 in UTC: a `Z` suffix and nothing Qt reads loosely.
    if (!issuedAt.endsWith(QLatin1Char('Z')) || !issuedAt.contains(QLatin1Char('T')))
        return PairingError::IssuedAtInvalid;
    const QDateTime issued = QDateTime::fromString(issuedAt, Qt::ISODateWithMs);
    if (!issued.isValid())
        return PairingError::IssuedAtInvalid;
    if (qAbs(issued.toMSecsSinceEpoch() - now.toMSecsSinceEpoch()) > issuedAtWindowMs)
        return PairingError::IssuedAtOutOfWindow;
    return std::nullopt;
}

std::optional<PairingError> checkSigned(const QByteArray& bytes, const QByteArray& signature,
                                        const VerifyingKey& key, int namedVersion,
                                        const QString& issuedAt, const QDateTime& now) {
    if (key.publicKey.size() != publicKeyBytes)
        return PairingError::InvalidKey;
    if (namedVersion != key.keyVersion)
        return PairingError::KeyVersionMismatch;
    if (const auto error = checkIssuedAt(issuedAt, now))
        return error;
    if (!cc::verifyEd25519(key.publicKey, bytes, signature))
        return PairingError::InvalidSignature;
    return std::nullopt;
}

} // namespace

QString issuedAtString(const QDateTime& now) { return now.toUTC().toString(Qt::ISODateWithMs); }

QString sdpDigest(const QString& sdp) {
    return QString::fromLatin1(
        QCryptographicHash::hash(sdp.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QByteArray RegisterPayload::bytes() const {
    return canonicalOf(header("runner-register"), {ticketId, workspaceId, machineId,
                                                   runnerKeyVersion, runnerFingerprint, issuedAt});
}

QByteArray ApprovalPayload::bytes() const {
    return canonicalOf(header("runner-pair-approve"), {ticketId, pairingDigest});
}

QByteArray ChallengePayload::bytes() const {
    return canonicalOf(header("runner-challenge"), {runnerId, runnerKeyVersion, nonce, issuedAt});
}

QByteArray OfferPayload::bytes() const {
    return canonicalOf(header("runner-offer"),
                       {connectionId, workspaceId, machineId, runnerId, runnerKeyVersion, jobId,
                        attempt, issuedAt, sdpDigest(sdp)});
}

QByteArray AnswerPayload::bytes() const {
    return canonicalOf(header("runner-answer"),
                       {connectionId, workspaceId, machineId, machineKeyVersion, runnerId, jobId,
                        sdpDigest(offerSdp), issuedAt, sdpDigest(sdp)});
}

QByteArray signPayload(const QByteArray& privateKey, const QByteArray& bytes) {
    return cc::signEd25519(privateKey, bytes);
}

std::optional<PairingError> verify(const RegisterPayload& payload, const QByteArray& signature,
                                   const VerifyingKey& key, const QDateTime& now) {
    if (const auto error = checkSigned(payload.bytes(), signature, key, payload.runnerKeyVersion,
                                       payload.issuedAt, now))
        return error;
    // The signed fingerprint must name the key that signed.
    if (!constantTimeEquals(runnerFingerprint(key.publicKey).toLatin1(),
                            payload.runnerFingerprint.toLatin1()))
        return PairingError::InvalidSignature;
    return std::nullopt;
}

std::optional<PairingError> verify(const ChallengePayload& payload, const QByteArray& signature,
                                   const VerifyingKey& key, const QDateTime& now) {
    return checkSigned(payload.bytes(), signature, key, payload.runnerKeyVersion, payload.issuedAt,
                       now);
}

std::optional<PairingError> verify(const OfferPayload& payload, const QByteArray& signature,
                                   const VerifyingKey& key, const QDateTime& now) {
    return checkSigned(payload.bytes(), signature, key, payload.runnerKeyVersion, payload.issuedAt,
                       now);
}

std::optional<PairingError> verify(const AnswerPayload& payload, const QByteArray& signature,
                                   const VerifyingKey& key, const QDateTime& now) {
    return checkSigned(payload.bytes(), signature, key, payload.machineKeyVersion, payload.issuedAt,
                       now);
}

} // namespace ketplus::runner::pairing
