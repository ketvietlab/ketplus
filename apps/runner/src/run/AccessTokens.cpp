#include "run/AccessTokens.h"

#include "Environment.h"
#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingSignatures.h"

#include <QRegularExpression>

namespace ketplus::runner::app {
namespace {

bool isNonce(const QString& nonce) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{22,128}$"));
    return pattern.match(nonce).hasMatch();
}

bool isSecretText(const QString& text) { return text.size() >= 32 && text.size() <= 512; }

AccessTokens::Result failed(const RelayResult& relay) {
    AccessTokens::Result result;
    result.relay = relay;
    if (relay.refused("runner_removed") || relay.refused("runner_not_found"))
        result.outcome = AccessTokens::Outcome::RunnerRemoved;
    else if (relay.refused("invalid_access"))
        result.outcome = AccessTokens::Outcome::InvalidAccess;
    else
        result.outcome = AccessTokens::Outcome::Retry;
    return result;
}

} // namespace

AccessTokens::Result AccessTokens::ensure() {
    if (!m_token.isEmpty() && m_environment.now().msecsTo(m_expiresAt) >= accessTokenMarginMs) {
        return {Outcome::Ok, {}, {}};
    }
    return renew();
}

AccessTokens::Result AccessTokens::renew() {
    m_token.clear();
    const RunnerCredential& credential = m_identity.credential;
    RelayResult challenge = m_relay.createChallenge(credential.runnerId);
    const QString nonce = challenge.body.value(QStringLiteral("nonce")).toString();
    if (challenge.ok() && !isNonce(nonce))
        challenge.kind = RelayResult::Kind::BadReply;
    if (!challenge.ok())
        return failed(challenge);

    pairing::ChallengePayload payload;
    payload.runnerId = credential.runnerId;
    payload.runnerKeyVersion = credential.keyVersion;
    payload.nonce = nonce;
    payload.issuedAt = pairing::issuedAtString(m_environment.now());
    const QByteArray signature = pairing::signPayload(m_identity.privateKey, payload.bytes());
    RelayResult issued = m_relay.issueCredential({
        {QStringLiteral("runnerId"), credential.runnerId},
        {QStringLiteral("keyVersion"), credential.keyVersion},
        {QStringLiteral("nonce"), nonce},
        {QStringLiteral("issuedAt"), payload.issuedAt},
        {QStringLiteral("signature"), QString::fromLatin1(control_crypto::base64Url(signature))},
        {QStringLiteral("refreshCredential"), credential.refreshCredential},
    });
    const QString token = issued.body.value(QStringLiteral("accessToken")).toString();
    const QString refresh = issued.body.value(QStringLiteral("refreshCredential")).toString();
    const QDateTime expiresAt = QDateTime::fromString(
        issued.body.value(QStringLiteral("expiresAt")).toString(), Qt::ISODateWithMs);
    if (issued.ok() && (!isSecretText(token) || !isSecretText(refresh) || !expiresAt.isValid()))
        issued.kind = RelayResult::Kind::BadReply;
    if (!issued.ok())
        return failed(issued);

    // The new credential is on disk before the token is used: the Relay accepts the previous
    // one only until the new one is first presented.
    RunnerCredential rotated = credential;
    rotated.refreshCredential = refresh;
    Result result;
    if (!m_state.saveCredential(rotated, &result.error)) {
        result.outcome = Outcome::StateFailure;
        return result;
    }
    m_identity.credential = rotated;
    m_token = token;
    m_expiresAt = expiresAt;
    result.outcome = Outcome::Ok;
    return result;
}

AccessTokens::Call
AccessTokens::call(const std::function<RelayResult(const QString& token)>& request) {
    Call call;
    call.token = ensure();
    if (call.token.outcome != Outcome::Ok)
        return call;
    call.relay = request(m_token);
    if (!call.relay.refused("invalid_access"))
        return call;
    call.token = renew();
    if (call.token.outcome != Outcome::Ok)
        return call;
    call.relay = request(m_token);
    return call;
}

AccessTokens::Call
AccessTokens::callOnce(const std::function<RelayResult(const QString& token)>& request) {
    Call call;
    call.token = ensure();
    if (call.token.outcome == Outcome::Ok)
        call.relay = request(m_token);
    return call;
}

} // namespace ketplus::runner::app
