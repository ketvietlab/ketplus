#include "peer/PeerSignals.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"
#include "transport/WebRtcPeerLink.h"

#include <QRegularExpression>

namespace ketplus::runner::peer {
namespace cc = ketplus::control_crypto;
namespace {

QString signatureText(const QByteArray& signature) {
    return QString::fromLatin1(cc::base64Url(signature));
}

bool isInteger(const QJsonValue& value) {
    return value.isDouble() && value.toDouble() == static_cast<double>(value.toInteger());
}

} // namespace

std::optional<QJsonObject> signedOffer(const OfferContext& context, const QString& sdp,
                                       const QDateTime& issuedAt, const OfferSigner& signer) {
    if (sdp.isEmpty() || sdp.toUtf8().size() > sdpLimit)
        return std::nullopt;
    pairing::OfferPayload payload;
    payload.connectionId = context.connectionId;
    payload.workspaceId = context.workspaceId;
    payload.machineId = context.machineId;
    payload.runnerId = context.runnerId;
    payload.runnerKeyVersion = context.runnerKeyVersion;
    payload.jobId = context.jobId;
    payload.attempt = context.attempt;
    payload.issuedAt = pairing::issuedAtString(issuedAt);
    payload.sdp = sdp;
    const QByteArray signature = signer.sign(payload.bytes());
    if (signature.size() != 64)
        return std::nullopt;
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("signal.offer")},
                       {QStringLiteral("connectionId"), context.connectionId},
                       {QStringLiteral("sdp"), sdp},
                       {QStringLiteral("jobId"), context.jobId},
                       {QStringLiteral("attempt"), context.attempt},
                       {QStringLiteral("runnerKeyVersion"), context.runnerKeyVersion},
                       {QStringLiteral("issuedAt"), payload.issuedAt},
                       {QStringLiteral("signature"), signatureText(signature)}};
}

QString sdpFingerprint(const QString& sdp) {
    static const QRegularExpression line(QStringLiteral("^a=fingerprint:(.+)$"),
                                         QRegularExpression::MultilineOption);
    QString found;
    for (auto it = line.globalMatch(sdp); it.hasNext();) {
        const QString normalized = daemon::transport::WebRtcPeerLink::normalizedFingerprint(
            it.next().captured(1).trimmed());
        if (normalized.isEmpty() || (!found.isEmpty() && found != normalized))
            return {};
        found = normalized;
    }
    return found;
}

QString answerErrorCode(const AnswerError error) {
    switch (error) {
    case AnswerError::Malformed:
        return QStringLiteral("invalid_answer");
    case AnswerError::NotPinned:
        return QStringLiteral("not_paired");
    case AnswerError::KeyVersionMismatch:
        return QStringLiteral("key_version_mismatch");
    case AnswerError::IssuedAt:
        return QStringLiteral("issued_at_invalid");
    case AnswerError::InvalidSignature:
        return QStringLiteral("invalid_signature");
    case AnswerError::Replayed:
        return QStringLiteral("answer_replayed");
    case AnswerError::NoFingerprint:
        return QStringLiteral("invalid_fingerprint");
    }
    return QStringLiteral("invalid_answer");
}

void UsedAnswers::expire(const QDateTime& now) {
    const qint64 at = now.toMSecsSinceEpoch();
    m_expiry.removeIf([at](const auto& entry) { return entry.value() <= at; });
}

bool UsedAnswers::contains(const QString& connectionId, const QDateTime& now) {
    expire(now);
    return m_expiry.contains(connectionId);
}

void UsedAnswers::add(const QString& connectionId, const QDateTime& now) {
    expire(now);
    if (m_expiry.size() >= usedAnswerLimit) {
        // The oldest goes first; ids are random, so a lost entry costs no safety in practice.
        auto oldest = m_expiry.begin();
        for (auto it = m_expiry.begin(); it != m_expiry.end(); ++it)
            if (it.value() < oldest.value())
                oldest = it;
        m_expiry.erase(oldest);
    }
    m_expiry.insert(connectionId, now.toMSecsSinceEpoch() + usedAnswerRetentionMs);
}

AnswerCheck checkAnswer(const QJsonObject& message, const OfferContext& context,
                        const QString& offerSdp, const std::optional<pairing::RunnerPin>& pin,
                        const QDateTime& now, UsedAnswers& used) {
    AnswerCheck result;
    const QJsonValue sdp = message.value(QStringLiteral("sdp"));
    const QJsonValue version = message.value(QStringLiteral("machineKeyVersion"));
    const QJsonValue issuedAt = message.value(QStringLiteral("issuedAt"));
    const QJsonValue signature = message.value(QStringLiteral("signature"));
    if (message.value(QStringLiteral("connectionId")) != context.connectionId || !sdp.isString() ||
        sdp.toString().isEmpty() || sdp.toString().toUtf8().size() > sdpLimit ||
        !isInteger(version) || !issuedAt.isString() || !signature.isString()) {
        result.error = AnswerError::Malformed;
        return result;
    }
    if (!pin || pin->state != pairing::RunnerPin::State::Active ||
        pin->machineId != context.machineId) {
        result.error = AnswerError::NotPinned;
        return result;
    }
    if (used.contains(context.connectionId, now)) {
        result.error = AnswerError::Replayed;
        return result;
    }
    pairing::AnswerPayload payload;
    payload.connectionId = context.connectionId;
    payload.workspaceId = pin->workspaceId;
    payload.machineId = pin->machineId;
    payload.machineKeyVersion = static_cast<int>(version.toInteger());
    payload.runnerId = context.runnerId;
    payload.jobId = context.jobId;
    payload.offerSdp = offerSdp;
    payload.issuedAt = issuedAt.toString();
    payload.sdp = sdp.toString();
    const auto error = pairing::verify(payload, cc::fromBase64Url(signature.toString()),
                                       {pin->signingPublicKey, pin->machineKeyVersion}, now);
    if (error) {
        switch (*error) {
        case pairing::PairingError::KeyVersionMismatch:
            result.error = AnswerError::KeyVersionMismatch;
            break;
        case pairing::PairingError::IssuedAtInvalid:
        case pairing::PairingError::IssuedAtOutOfWindow:
            result.error = AnswerError::IssuedAt;
            break;
        default:
            result.error = AnswerError::InvalidSignature;
        }
        return result;
    }
    const QString fingerprint = sdpFingerprint(payload.sdp);
    if (fingerprint.isEmpty()) {
        result.error = AnswerError::NoFingerprint;
        return result;
    }
    used.add(context.connectionId, now);
    result.answer = VerifiedAnswer{payload.sdp, fingerprint};
    return result;
}

} // namespace ketplus::runner::peer
