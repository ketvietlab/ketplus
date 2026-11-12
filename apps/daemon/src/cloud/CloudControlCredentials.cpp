#include "cloud/CloudControlClient.h"
#include "cloud/CloudControlSupport.h"
#include "cloud/MachineChallenge.h"

#include <utility>

namespace ketplus::daemon {
using cloud_control::object;
using cloud_control::responseCode;

void CloudControlClient::ensureDaemonCredential(ReadyCallback callback) {
    if (!daemonToken_.isEmpty() &&
        daemonTokenExpiresAt_ > QDateTime::currentDateTimeUtc().addSecs(60)) {
        callback(true, {});
        return;
    }
    credentialWaiters_.append(std::move(callback));
    if (credentialRequestInFlight_)
        return;
    if (machineId_.isEmpty()) {
        finishCredentialWaiters(false, QStringLiteral("This Mac has not been enrolled."));
        return;
    }
    credentialRequestInFlight_ = true;
    const quint64 requestEpoch = accountEpoch_;
    const QJsonObject challengeRequest{
        {QStringLiteral("machineId"), machineId_},
        {QStringLiteral("keyVersion"), machineKeyVersion_},
        {QStringLiteral("signatureVersion"), 2},
        {QStringLiteral("signatureAlgorithm"), QStringLiteral("Ed25519")}};
    request(
        "POST", QStringLiteral("/v2/daemon/challenges"), challengeRequest, {}, {},
        [this, requestEpoch](const int status, const QJsonValue& value,
                             const QString& transportError) {
            if (requestEpoch != accountEpoch_)
                return;
            const QJsonObject challenge = object(value);
            if (status == 404 && transportError.isEmpty() &&
                responseCode(value, {}) == QStringLiteral("machine_not_found")) {
                // A removed/revoked enrollment cannot acquire another credential. Leave the
                // account once instead of retrying the same missing machine on every timer tick.
                // The normal sign-out also closes peer/Atlas access and preserves local work.
                signOut([this](const QJsonObject&, const QString& error) {
                    if (error.isEmpty())
                        return;
                    // Disk failure must not keep a revoked identity connected in this process.
                    wakeups_.stop();
                    machineId_.clear();
                    workspaceId_.clear();
                    accountName_.clear();
                    accountUsername_.clear();
                    daemonToken_.clear();
                    finishCredentialWaiters(false, error);
                });
                return;
            }
            if (status != 201 || !transportError.isEmpty()) {
                finishCredentialWaiters(
                    false, transportError.isEmpty()
                               ? responseCode(value, QStringLiteral("challenge_failed"))
                               : transportError);
                return;
            }
            const QByteArray bytes =
                machine_challenge::signingBytes(challenge, machineId_, machineKeyVersion_);
            if (bytes.isEmpty()) {
                finishCredentialWaiters(
                    false, QStringLiteral("The relay sent an invalid machine challenge."));
                return;
            }
            QString cryptoError;
            const QByteArray signature = crypto_.sign(bytes, &cryptoError);
            if (signature.isEmpty()) {
                finishCredentialWaiters(false, cryptoError);
                return;
            }
            const QJsonObject body{
                {QStringLiteral("challengeId"), challenge.value(QStringLiteral("id"))},
                {QStringLiteral("signatureVersion"), 2},
                {QStringLiteral("signatureAlgorithm"), QStringLiteral("Ed25519")}};
            request(
                "POST", QStringLiteral("/v2/daemon/credentials"), body, {},
                {{QByteArray("X-KetPlus-Machine-Signature"), CloudCrypto::base64Url(signature)}},
                [this, requestEpoch](const int credentialStatus, const QJsonValue& credentialValue,
                                     const QString& credentialError) {
                    if (requestEpoch != accountEpoch_)
                        return;
                    const QJsonObject credential = object(credentialValue);
                    if (credentialStatus != 201 || !credentialError.isEmpty()) {
                        finishCredentialWaiters(
                            false,
                            credentialError.isEmpty()
                                ? responseCode(credentialValue, QStringLiteral("credential_failed"))
                                : credentialError);
                        return;
                    }
                    daemonToken_ = credential.value(QStringLiteral("accessToken")).toString();
                    daemonTokenExpiresAt_ = QDateTime::fromString(
                        credential.value(QStringLiteral("expiresAt")).toString(), Qt::ISODate);
                    finishCredentialWaiters(!daemonToken_.isEmpty(),
                                            daemonToken_.isEmpty()
                                                ? QStringLiteral("Missing daemon credential.")
                                                : QString{});
                });
        });
}

void CloudControlClient::finishCredentialWaiters(const bool ready, const QString& error) {
    credentialRequestInFlight_ = false;
    if (!ready) {
        daemonToken_.clear();
        lastError_ = error;
    } else {
        lastError_.clear();
    }
    const auto waiters = std::exchange(credentialWaiters_, {});
    for (const ReadyCallback& waiter : waiters)
        waiter(ready, error);
}

} // namespace ketplus::daemon
