#include "cloud/CloudControlClient.h"
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>
namespace ketplus::daemon {
namespace {
QString randomSecret() {
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(bytes.begin(), bytes.end());
    return QString::fromLatin1(CloudCrypto::base64Url(bytes));
}
} // namespace
void CloudControlClient::signInDevice(LocalCallback callback) {
    const auto epoch = ++accountEpoch_;
    dropBrowserSignIn();
    signInError_.clear();
    auto installation = storedString("installation-id");
    if (installation.isEmpty()) {
        installation = "host_" + QUuid::createUuid().toString(QUuid::Id128);
        if (!storeString("installation-id", installation)) {
            callback({}, "Unable to store the host installation identity.");
            return;
        }
    }
    QString failure;
    const auto keys = crypto_.publicKeys(&failure);
    if (keys.isEmpty()) {
        callback({}, failure);
        return;
    }
    const auto verifier = randomSecret();
    const auto challenge = QString::fromLatin1(CloudCrypto::base64Url(
        QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)));
    deviceSignInStarting_ = true;
    request(
        "POST", "/v2/auth/device/code",
        QJsonObject{{"installation_id", installation}, {"code_challenge", challenge}}, {}, {},
        [this, epoch, installation, keys, verifier,
         callback = std::move(callback)](int code, const QJsonValue& body, const QString& error) {
            if (epoch != accountEpoch_) {
                callback({}, "Sign-in was canceled.");
                return;
            }
            deviceSignInStarting_ = false;
            const auto value = body.toObject();
            const QUrl page(value.value("verification_uri_complete").toString());
            const auto secret = value.value("device_code").toString();
            const auto seconds = value.value("expires_in").toInt();
            const auto interval = value.value("interval").toInt();
            const bool validCodes =
                QRegularExpression("^ketplus_device_[A-Za-z0-9_-]{43}$").match(secret).hasMatch() &&
                QRegularExpression("^[A-F0-9]{8}$")
                    .match(value.value("user_code").toString())
                    .hasMatch();
            if (code != 200 || !error.isEmpty() || !validCodes || seconds < 1 || seconds > 600 ||
                interval < 5 || interval > 60 || !page.isValid() ||
                page.scheme() != baseUrl_.scheme() || page.host() != baseUrl_.host() ||
                page.port(-1) != baseUrl_.port(-1) || !page.userInfo().isEmpty()) {
                signInError_ = error.isEmpty()
                                   ? QStringLiteral("Invalid device authorization response.")
                                   : error;
                callback({}, signInError_);
                return;
            }
            deviceCode_ = secret;
            deviceVerifier_ = verifier;
            deviceKeys_ = keys;
            deviceInstallationId_ = installation;
            deviceSignInExpires_ = QDateTime::currentDateTimeUtc().addSecs(seconds);
            devicePollInterval_ = interval * 1000;
            deviceSignInTimer_.setSingleShot(true);
            deviceSignInTimer_.disconnect(this);
            connect(&deviceSignInTimer_, &QTimer::timeout, this,
                    [this, epoch] { pollDeviceSignIn(epoch); });
            deviceSignInTimer_.start(devicePollInterval_);
            callback({{"ok", true},
                      {"authorization_url", page.toString()},
                      {"user_code", value.value("user_code")},
                      {"expires_in", seconds}},
                     {});
        });
}
void CloudControlClient::pollDeviceSignIn(quint64 epoch) {
    if (epoch != accountEpoch_ || deviceCode_.isEmpty() || devicePollInFlight_)
        return;
    if (QDateTime::currentDateTimeUtc() >= deviceSignInExpires_) {
        finishSignIn(epoch, "Device authorization expired. Start sign-in again.");
        return;
    }
    devicePollInFlight_ = true;
    request("POST", "/v2/auth/device/token",
            QJsonObject{{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                        {"device_code", deviceCode_},
                        {"code_verifier", deviceVerifier_},
                        {"installation_id", deviceInstallationId_}},
            {}, {}, [this, epoch](int code, const QJsonValue& body, const QString& error) {
                if (epoch != accountEpoch_ || deviceCode_.isEmpty())
                    return;
                devicePollInFlight_ = false;
                const auto value = body.toObject();
                const auto reason = value.value("error").toString(value.value("code").toString());
                if (!error.isEmpty() || code == 0 || code >= 500 ||
                    reason == "authorization_pending" || reason == "slow_down") {
                    if (reason == "slow_down")
                        devicePollInterval_ = qMin(60000, devicePollInterval_ + 5000);
                    deviceSignInTimer_.start(devicePollInterval_);
                    return;
                }
                const auto access = value.value("accessToken").toString();
                if (code != 200 || access.isEmpty()) {
                    finishSignIn(epoch, reason.isEmpty()
                                            ? QStringLiteral("Device authorization failed.")
                                            : reason);
                    return;
                }
                enrollMachine(epoch, access, value.value("subject").toObject(),
                              deviceInstallationId_, deviceKeys_,
                              [this, epoch](const QJsonObject&, const QString& failure) {
                                  finishSignIn(epoch, failure);
                              });
            });
}
} // namespace ketplus::daemon
