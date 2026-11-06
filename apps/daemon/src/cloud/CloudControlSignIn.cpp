// Signing this Mac in to its relay through the system browser, then enrolling it.
//
// The daemon never sees a password. remote.sign_in starts a handoff and answers with the page
// the desktop opens in the browser; the relay's tenant signs the viewer in there and sends the
// browser back to BrowserSignIn's loopback port. The code it brings is redeemed once, with the
// PKCE verifier, for a short user session that is used only to enroll this Mac. remote.status
// reports the sign-in while it waits and why it failed.

#include "cloud/BrowserSignIn.h"
#include "cloud/CloudControlClient.h"
#include "cloud/HostPlatform.h"
#include "storage/LocalJournal.h"

#include <QHostInfo>
#include <QUuid>

namespace ketplus::daemon {
namespace {

QJsonObject object(const QJsonValue& value) {
    return value.isObject() ? value.toObject() : QJsonObject{};
}

QString responseCode(const QJsonValue& value, const QString& fallback) {
    return object(value).value(QStringLiteral("code")).toString(fallback);
}

const QString canceled = QStringLiteral("Sign-in was canceled.");

} // namespace

void CloudControlClient::signIn(LocalCallback callback) {
    const quint64 signInEpoch = ++accountEpoch_;
    dropBrowserSignIn();
    signInError_.clear();
    QString installationId = storedString(QStringLiteral("installation-id"));
    if (installationId.isEmpty()) {
        installationId = QStringLiteral("host_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-'));
        if (!storeString(QStringLiteral("installation-id"), installationId)) {
            callback({}, QStringLiteral("Unable to store the host installation identity."));
            return;
        }
    }
    QString failure;
    const QJsonObject keys = crypto_.publicKeys(&failure);
    if (keys.isEmpty()) {
        callback({}, failure);
        return;
    }
    auto* browser = new BrowserSignIn(this);
    if (!browser->listen(&failure)) {
        browser->deleteLater();
        callback({}, failure);
        return;
    }
    browserSignIn_ = browser;
    const QJsonObject handoff{{QStringLiteral("installationId"), installationId},
                              {QStringLiteral("redirectUri"), browser->redirectUri()},
                              {QStringLiteral("state"), browser->state()},
                              {QStringLiteral("codeChallenge"), browser->codeChallenge()}};
    request("POST", QStringLiteral("/v2/auth/handoffs"), handoff, {}, {},
            [this, signInEpoch, browser, installationId, keys, callback = std::move(callback)](
                const int status, const QJsonValue& value, const QString& transportError) {
                if (signInEpoch != accountEpoch_ || browserSignIn_ != browser) {
                    callback({}, canceled);
                    return;
                }
                const QUrl page(object(value).value(QStringLiteral("authorizationUrl")).toString());
                if (status != 200 || !transportError.isEmpty() || !page.isValid()) {
                    dropBrowserSignIn();
                    callback({}, transportError.isEmpty()
                                     ? responseCode(value, QStringLiteral("sign_in_unavailable"))
                                     : transportError);
                    return;
                }
                // The desktop opens this page in the browser: it must be this relay's own.
                if (page.scheme() != baseUrl_.scheme() || page.host() != baseUrl_.host() ||
                    page.port(-1) != baseUrl_.port(-1)) {
                    dropBrowserSignIn();
                    callback({}, QStringLiteral("The relay named a sign-in page on another host."));
                    return;
                }
                connect(browser, &BrowserSignIn::failed, this,
                        [this, signInEpoch](const QString& error) {
                            finishSignIn(signInEpoch, error);
                        });
                connect(browser, &BrowserSignIn::codeReceived, this,
                        [this, signInEpoch, browser, installationId, keys](const QString& code) {
                            redeemSignIn(signInEpoch, code, browser->codeVerifier(),
                                         browser->redirectUri(), installationId, keys);
                        });
                callback({{QStringLiteral("ok"), true},
                          {QStringLiteral("authorization_url"), page.toString(QUrl::FullyEncoded)}},
                         {});
            });
}

void CloudControlClient::cancelSignIn(LocalCallback callback) {
    if (browserSignIn_ || deviceSignInStarting_ || !deviceCode_.isEmpty()) {
        ++accountEpoch_;
        dropBrowserSignIn();
    }
    signInError_.clear();
    callback(status(), {});
}

void CloudControlClient::dropBrowserSignIn() {
    deviceSignInTimer_.stop();
    deviceSignInStarting_ = false;
    deviceCode_.clear();
    deviceVerifier_.clear();
    deviceInstallationId_.clear();
    deviceKeys_ = {};
    devicePollInFlight_ = false;
    if (browserSignIn_) {
        browserSignIn_->disconnect(this);
        browserSignIn_->deleteLater();
    }
    browserSignIn_.clear();
}

void CloudControlClient::finishSignIn(const quint64 signInEpoch, const QString& error) {
    if (signInEpoch != accountEpoch_)
        return;
    dropBrowserSignIn();
    signInError_ = error;
}

void CloudControlClient::redeemSignIn(const quint64 signInEpoch, const QString& code,
                                      const QString& verifier, const QString& redirectUri,
                                      const QString& installationId, const QJsonObject& keys) {
    const QJsonObject redemption{{QStringLiteral("code"), code},
                                 {QStringLiteral("codeVerifier"), verifier},
                                 {QStringLiteral("redirectUri"), redirectUri},
                                 {QStringLiteral("installationId"), installationId}};
    request(
        "POST", QStringLiteral("/v2/auth/token"), redemption, {}, {},
        [this, signInEpoch, installationId, keys](const int status, const QJsonValue& value,
                                                  const QString& transportError) {
            if (signInEpoch != accountEpoch_)
                return;
            const QString access = object(value).value(QStringLiteral("accessToken")).toString();
            if (status != 200 || access.isEmpty() || !transportError.isEmpty()) {
                finishSignIn(signInEpoch, transportError.isEmpty()
                                              ? responseCode(value, QStringLiteral("invalid_grant"))
                                              : transportError);
                return;
            }
            enrollMachine(signInEpoch, access, installationId, keys,
                          [this, signInEpoch](const QJsonObject&, const QString& error) {
                              finishSignIn(signInEpoch, error);
                          });
        });
}

void CloudControlClient::enrollMachine(const quint64 signInEpoch, const QString& access,
                                       const QString& installationId, const QJsonObject& keys,
                                       LocalCallback callback) {
    const QJsonObject enrollment{
        {QStringLiteral("installationId"), installationId},
        {QStringLiteral("displayName"), QHostInfo::localHostName().left(128)},
        {QStringLiteral("platform"), hostPlatform()},
        {QStringLiteral("keys"), keys}};
    request(
        "POST", QStringLiteral("/v2/machines:enroll"), enrollment, access, {},
        [this, signInEpoch, callback = std::move(callback)](
            const int enrollStatus, const QJsonValue& enrollValue, const QString& enrollError) {
            if (signInEpoch != accountEpoch_) {
                callback({}, canceled);
                return;
            }
            const QJsonObject machine = object(enrollValue);
            if (enrollStatus != 201 || !enrollError.isEmpty()) {
                callback({}, enrollError.isEmpty()
                                 ? responseCode(enrollValue, QStringLiteral("enrollment_failed"))
                                 : enrollError);
                return;
            }
            QString storageError;
            // Upload progress recorded against another relay would resume this one
            // mid-history (see signOut).
            if (!otherRelay_.isEmpty() && !clearUploadProgress(&storageError)) {
                callback({}, storageError);
                return;
            }
            otherRelay_.clear();
            signaling_->configure({}, {}, 1, false);
            const QString machineId = machine.value(QStringLiteral("id")).toString();
            const QString workspaceId = machine.value(QStringLiteral("workspaceId")).toString();
            const int machineKeyVersion = machine.value(QStringLiteral("keyVersion")).toInt(1);
            // A new enrollment trusts no device yet: only an existing enrollment adopts the
            // devices it already served when pins were introduced (ADR 0034). The empty pin set
            // is written before the machine id, and the enrollment takes effect in memory only
            // once everything is stored, so a failed write never leaves an enrollment that this
            // daemon or a restart would mistake for one from before pins.
            const bool stored =
                pins_.reset(&storageError) &&
                storeString(QStringLiteral("control-url"), baseUrl_.toString(), &storageError) &&
                storeString(QStringLiteral("machine-id"), machineId, &storageError) &&
                storeString(QStringLiteral("workspace-id"), workspaceId, &storageError) &&
                storeString(QStringLiteral("machine-key-version"),
                            QString::number(machineKeyVersion), &storageError);
            if (!stored) {
                callback({}, storageError);
                return;
            }
            machineId_ = machineId;
            workspaceId_ = workspaceId;
            machineKeyVersion_ = machineKeyVersion;
            if (!registerCloudCursors()) {
                callback({}, lastError_);
                return;
            }
            daemonToken_.clear();
            ensureDaemonCredential(
                [this, callback = std::move(callback)](const bool ready, const QString& error) {
                    if (!ready) {
                        callback({}, error);
                        return;
                    }
                    callback(status(), {});
                    sync();
                    pollCommands();
                    wakeups_.start();
                });
        });
}

} // namespace ketplus::daemon
