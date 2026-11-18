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
    invalidateSenderKeys();
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
        invalidateSenderKeys();
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
            enrollMachine(signInEpoch, access,
                          object(value).value(QStringLiteral("subject")).toObject(), installationId,
                          keys, [this, signInEpoch](const QJsonObject&, const QString& error) {
                              finishSignIn(signInEpoch, error);
                          });
        });
}

void CloudControlClient::restoreEnrollment() {
    // Validate every read before migration writes or granting an in-memory identity. In
    // particular, an unavailable pin/relay record is not a missing legacy record.
    QHash<QString, QByteArray> values;
    QString error;
    for (const auto* field : {"machine-id", "workspace-id", "account-name", "account-username",
                              "machine-key-version", "control-url", "device-pins"}) {
        values.insert(QLatin1String(field), credentials_.read(QLatin1String(field), &error));
        if (!error.isEmpty()) {
            lastError_ = error;
            return;
        }
    }
    const bool pinsInitialized = pins_.initialized(&error);
    if (!error.isEmpty()) {
        lastError_ = error;
        return;
    }
    const auto machine = QString::fromUtf8(values.value("machine-id"));
    const auto workspace = QString::fromUtf8(values.value("workspace-id"));
    if (machine.isEmpty())
        return;
    if (workspace.isEmpty()) {
        lastError_ = QStringLiteral("Stored enrollment has no workspace; sign in again.");
        return;
    }
    const auto relay = QString::fromUtf8(values.value("control-url"));
    if (!relay.isEmpty() && relay != baseUrl_.toString()) {
        otherRelay_ = relay;
        lastError_ =
            QStringLiteral("This Mac is enrolled with %1, not %2.").arg(relay, baseUrl_.toString());
        return;
    }
    bool changedDuringRestore = false;
    if (!credentials_.update(
            [&](QHash<QString, QByteArray>& accounts) {
                // Another cooperating process may have changed the record between reads.
                // Never apply legacy migration to an identity other than the one validated.
                for (auto it = values.cbegin(); it != values.cend(); ++it) {
                    if (accounts.value(it.key()) != it.value()) {
                        changedDuringRestore = true;
                        return;
                    }
                }
                SessionCredentials::adoptWorkspace(accounts, workspace);
                if (relay.isEmpty())
                    accounts.insert(QStringLiteral("control-url"), baseUrl_.toString().toUtf8());
            },
            &error)) {
        lastError_ = error;
        return;
    }
    if (changedDuringRestore) {
        lastError_ = QStringLiteral("Credential storage changed during enrollment restore; retry.");
        return;
    }
    machineId_ = machine;
    workspaceId_ = workspace;
    accountName_ = QString::fromUtf8(values.value("account-name"));
    accountUsername_ = QString::fromUtf8(values.value("account-username"));
    machineKeyVersion_ = qMax(1, values.value("machine-key-version").toInt());
    legacyPinAdoptionPending_ = !pinsInitialized;
    registerCloudCursors();
}

void CloudControlClient::enrollMachine(const quint64 signInEpoch, const QString& access,
                                       const QJsonObject& subject, const QString& installationId,
                                       const QJsonObject& keys, LocalCallback callback) {
    legacyPinAdoptionPending_ = false;
    const QJsonObject enrollment{
        {QStringLiteral("installationId"), installationId},
        {QStringLiteral("displayName"), QHostInfo::localHostName().left(128)},
        {QStringLiteral("platform"), hostPlatform()},
        {QStringLiteral("keys"), keys}};
    request(
        "POST", QStringLiteral("/v2/machines:enroll"), enrollment, access, {},
        [this, signInEpoch, subject, callback = std::move(callback)](
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
            const QString machineId = machine.value(QStringLiteral("id")).toString();
            const QString workspaceId = machine.value(QStringLiteral("workspaceId")).toString();
            const int machineKeyVersion = machine.value(QStringLiteral("keyVersion")).toInt(1);
            // Shown in Settings and Connections; the relay's user session is not kept.
            const QString accountName =
                subject.value(QStringLiteral("displayName")).toString().left(128);
            const QString accountUsername =
                subject.value(QStringLiteral("username")).toString().left(128);
            // Fence before the transaction, even on storage failure. Preserve the sign-in
            // epoch so its completion callback can still finish. Existing Atlas access and
            // credentials cannot survive a same-identity enrollment either.
            ++enrollmentRevision_;
            invalidateSenderKeys();
            revokeAtlasAccess();
            signaling_->configure({}, {}, 1, false);
            wakeups_.stop();
            daemonToken_.clear();
            daemonTokenExpiresAt_ = {};
            const bool stored = credentials_.update(
                [&](QHash<QString, QByteArray>& accounts) {
                    if (!otherRelay_.isEmpty())
                        accounts.removeIf([](const auto& entry) {
                            return SessionCredentials::isUploadCursor(entry.key());
                        });
                    accounts.insert(QStringLiteral("device-pins"),
                                    QByteArrayLiteral("{\"version\":1,\"devices\":{}}"));
                    // A new enrollment trusts no runner the old one paired.
                    accounts.remove(QStringLiteral("runner-pins"));
                    accounts.insert(QStringLiteral("control-url"), baseUrl_.toString().toUtf8());
                    accounts.insert(QStringLiteral("machine-id"), machineId.toUtf8());
                    accounts.insert(QStringLiteral("workspace-id"), workspaceId.toUtf8());
                    accounts.insert(QStringLiteral("machine-key-version"),
                                    QByteArray::number(machineKeyVersion));
                    accounts.insert(QStringLiteral("account-name"), accountName.toUtf8());
                    accounts.insert(QStringLiteral("account-username"), accountUsername.toUtf8());
                    SessionCredentials::adoptWorkspace(accounts, workspaceId);
                },
                &storageError);
            if (!stored) {
                finishCredentialWaiters(false, storageError);
                callback({}, storageError);
                return;
            }
            otherRelay_.clear();
            machineId_ = machineId;
            workspaceId_ = workspaceId;
            accountName_ = accountName;
            accountUsername_ = accountUsername;
            machineKeyVersion_ = machineKeyVersion;
            finishCredentialWaiters(false, QStringLiteral("Enrollment changed."));
            if (signInEpoch != accountEpoch_) {
                callback({}, canceled);
                return;
            }
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
