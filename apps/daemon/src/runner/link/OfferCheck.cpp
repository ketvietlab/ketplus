#include "runner/link/OfferCheck.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"
#include "protocol/FrameRules.h"

#include <QRegularExpression>

namespace ketplus::daemon::runner::link {
namespace cc = ketplus::control_crypto;
namespace pairing = ketplus::runner::pairing;
namespace rules = ketplus::runner::protocol::rules;
namespace {

constexpr qint64 intLimit = 2147483647;

bool isInteger(const QJsonValue& value) {
    return value.isDouble() && value.toDouble() == static_cast<double>(value.toInteger());
}

bool isPositiveInt(const QJsonValue& value) {
    return isInteger(value) && value.toInteger() >= 1 && value.toInteger() <= intLimit;
}

bool isSdp(const QJsonValue& value) {
    return value.isString() && !value.toString().isEmpty() &&
           value.toString().toUtf8().size() <= sdpLimit;
}

// RFC 3339 in UTC, as the pairing checks read it.
std::optional<QDateTime> issuedAtTime(const QString& text) {
    if (!text.endsWith(QLatin1Char('Z')) || !text.contains(QLatin1Char('T')))
        return std::nullopt;
    const QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!time.isValid())
        return std::nullopt;
    return time;
}

} // namespace

QString offerErrorReason(const OfferError error) {
    switch (error) {
    case OfferError::Unreadable:
    case OfferError::Replayed:
        return {};
    case OfferError::Malformed:
        return QStringLiteral("invalid_offer");
    case OfferError::NotPinned:
    case OfferError::KeyVersionMismatch:
        return QStringLiteral("not_paired");
    case OfferError::InvalidSignature:
        return QStringLiteral("invalid_signature");
    case OfferError::Expired:
        return QStringLiteral("offer_expired");
    case OfferError::UsedFull:
        return QStringLiteral("peer_limit");
    }
    return QStringLiteral("invalid_offer");
}

void UsedOffers::expire(const QDateTime& now) {
    const qint64 at = now.toMSecsSinceEpoch();
    m_expiry.removeIf([at](const auto& entry) { return entry.value() <= at; });
}

bool UsedOffers::contains(const QString& connectionId, const QDateTime& now) {
    expire(now);
    return m_expiry.contains(connectionId);
}

bool UsedOffers::add(const QString& connectionId, const QDateTime& now) {
    expire(now);
    if (m_expiry.size() >= usedOfferLimit)
        return false;
    m_expiry.insert(connectionId, now.toMSecsSinceEpoch() + usedOfferRetentionMs);
    return true;
}

bool isRunnerOffer(const QJsonObject& message) {
    return message.value(QStringLiteral("type")) == QLatin1StringView("signal.offer") &&
           message.value(QStringLiteral("runnerId")).isString() &&
           message.value(QStringLiteral("jobId")).isString() &&
           !message.contains(QStringLiteral("grant")) &&
           !message.contains(QStringLiteral("deviceId"));
}

OfferCheck checkOffer(const QJsonObject& message, const MachineIdentity& identity,
                      const RunnerPins& pins, const QDateTime& now, UsedOffers& used) {
    static const QRegularExpression idPattern(QString::fromLatin1(connectionIdPattern));
    OfferCheck result;
    const QJsonValue id = message.value(QStringLiteral("connectionId"));
    if (!id.isString() || !idPattern.match(id.toString()).hasMatch())
        return result;
    result.connectionId = id.toString();
    if (used.contains(result.connectionId, now)) {
        result.error = OfferError::Replayed;
        return result;
    }

    const QJsonValue runnerId = message.value(QStringLiteral("runnerId"));
    const QJsonValue version = message.value(QStringLiteral("runnerKeyVersion"));
    const QJsonValue jobId = message.value(QStringLiteral("jobId"));
    const QJsonValue attempt = message.value(QStringLiteral("attempt"));
    const QJsonValue sdp = message.value(QStringLiteral("sdp"));
    const QJsonValue issuedAt = message.value(QStringLiteral("issuedAt"));
    const QJsonValue signature = message.value(QStringLiteral("signature"));
    const QJsonValue iceServers = message.value(QStringLiteral("iceServers"));
    const std::optional<QDateTime> issued =
        issuedAt.isString() ? issuedAtTime(issuedAt.toString()) : std::nullopt;
    if (!runnerId.isString() || !rules::isToken(runnerId.toString(), idLimit) ||
        !isPositiveInt(version) || !jobId.isString() || !rules::isJobId(jobId.toString()) ||
        !isPositiveInt(attempt) || !rules::isAttempt(attempt.toInteger()) || !isSdp(sdp) ||
        !issued || !signature.isString() || !(iceServers.isArray() || iceServers.isUndefined())) {
        result.error = OfferError::Malformed;
        return result;
    }

    const auto pin = pins.pin(runnerId.toString());
    if (!pin || pin->runnerId != runnerId.toString()) {
        result.error = OfferError::NotPinned;
        return result;
    }
    if (version.toInteger() != pin->runnerKeyVersion) {
        result.error = OfferError::KeyVersionMismatch;
        return result;
    }

    pairing::OfferPayload payload;
    payload.connectionId = result.connectionId;
    payload.workspaceId = identity.workspaceId();
    payload.machineId = identity.machineId();
    payload.runnerId = runnerId.toString();
    payload.runnerKeyVersion = static_cast<int>(version.toInteger());
    payload.jobId = jobId.toString();
    payload.attempt = static_cast<int>(attempt.toInteger());
    payload.issuedAt = issuedAt.toString();
    payload.sdp = sdp.toString();
    // The signature first, against the offer's own time; the clock window after it.
    if (pairing::verify(payload, cc::fromBase64Url(signature.toString()),
                        {pin->publicKey, pin->runnerKeyVersion}, *issued)) {
        result.error = OfferError::InvalidSignature;
        return result;
    }
    if (qAbs(issued->toMSecsSinceEpoch() - now.toMSecsSinceEpoch()) > pairing::issuedAtWindowMs) {
        result.error = OfferError::Expired;
        return result;
    }
    if (!used.add(result.connectionId, now)) {
        result.error = OfferError::UsedFull;
        return result;
    }

    result.offer = VerifiedOffer{result.connectionId, payload.runnerId, payload.runnerKeyVersion,
                                 payload.jobId,       payload.attempt,  payload.sdp,
                                 iceServers.toArray()};
    return result;
}

std::optional<QJsonObject> signedAnswer(const VerifiedOffer& offer, const QString& sdp,
                                        const QDateTime& issuedAt,
                                        const MachineIdentity& identity) {
    if (sdp.isEmpty() || sdp.toUtf8().size() > sdpLimit)
        return std::nullopt;
    pairing::AnswerPayload payload;
    payload.connectionId = offer.connectionId;
    payload.workspaceId = identity.workspaceId();
    payload.machineId = identity.machineId();
    payload.machineKeyVersion = identity.machineKeyVersion();
    payload.runnerId = offer.runnerId;
    payload.jobId = offer.jobId;
    payload.offerSdp = offer.sdp;
    payload.issuedAt = pairing::issuedAtString(issuedAt);
    payload.sdp = sdp;
    const QByteArray signature = identity.sign(payload.bytes());
    if (signature.size() != 64)
        return std::nullopt;
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("signal.answer")},
        {QStringLiteral("connectionId"), offer.connectionId},
        {QStringLiteral("sdp"), sdp},
        {QStringLiteral("machineKeyVersion"), payload.machineKeyVersion},
        {QStringLiteral("issuedAt"), payload.issuedAt},
        {QStringLiteral("signature"), QString::fromLatin1(cc::base64Url(signature))}};
}

} // namespace ketplus::daemon::runner::link
