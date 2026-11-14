#include "cloud/BrowserSignIn.h"
#include "cloud/HostPlatform.h"
#include "control/ControlV2Client.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <utility>
namespace ketplus::remote {
namespace {
bool validCredential(const QJsonObject& value) {
    const QRegularExpression token("^[A-Za-z0-9_-]{48,256}$");
    return token.match(value.value("accessToken").toString()).hasMatch() &&
           token.match(value.value("refreshToken").toString()).hasMatch() &&
           value.value("tokenType").toString() == "Bearer" &&
           value.value("expiresIn").toInt() >= 60 && value.value("expiresIn").toInt() <= 3600 &&
           !value.value("sessionId").toString().isEmpty() &&
           !value.value("subject").toObject().value("id").toString().isEmpty();
}
QJsonObject withDeadline(QJsonObject value) {
    value.insert("expiresAt", QDateTime::currentDateTimeUtc()
                                  .addSecs(value.value("expiresIn").toInt())
                                  .toString(Qt::ISODateWithMs));
    return value;
}
} // namespace
bool ControlV2Client::setCredential(const QJsonObject& credential, QString* error) {
    if (!validCredential(credential) || identity(error).isEmpty()) {
        if (error && error->isEmpty())
            *error = "Invalid browser credential session.";
        return false;
    }
    auto value = withDeadline(credential);
    value.remove("deviceId");
    value.remove("deviceKeyVersion");
    // Another account starts unpaired: no host pins carry over. The pin set is emptied, not
    // removed, and before the credential is replaced: if that write fails, the old pairing
    // stays with no pins rather than one that would adopt the next host keys it reads (#88).
    if (!storeHostPins({}, error))
        return false;
    if (!credentials_.write("user-session", QJsonDocument(value).toJson(QJsonDocument::Compact),
                            error))
        return false;
    ++epoch_;
    signerVersions_.clear();
    credential_ = value;
    credentialUnsaved_ = false;
    deviceId_.clear();
    pairingClaims_.clear();
    pairingHosts_.clear();
    refreshing_ = false;
    ++pairingEpoch_;
    credentials_.remove("refresh-operation");
    const auto waiters = std::exchange(refreshWaiters_, {});
    for (const auto& done : waiters)
        done({}, "Account changed.");
    return true;
}

void ControlV2Client::cancelSignIn() {
    ++signInEpoch_;
    if (browserSignIn_) {
        browserSignIn_->disconnect(this);
        browserSignIn_->deleteLater();
        browserSignIn_.clear();
    }
}

void ControlV2Client::startSignIn(Callback done) {
    cancelSignIn();
    const auto attempt = signInEpoch_;
    QString failure;
    const auto own = identity(&failure);
    if (own.isEmpty()) {
        done({},
             failure.isEmpty() ? QStringLiteral("Unable to create Desktop identity.") : failure);
        return;
    }
    auto* browser = new daemon::BrowserSignIn(this);
    if (!browser->listen(&failure)) {
        browser->deleteLater();
        done({}, failure);
        return;
    }
    browserSignIn_ = browser;
    const auto handoff = QJsonObject{{"installationId", own.value("installationId")},
                                     {"redirectUri", browser->redirectUri()},
                                     {"state", browser->state()},
                                     {"codeChallenge", browser->codeChallenge()}};
    request(
        "POST", "/v2/auth/handoffs", handoff, {},
        [this, browser, attempt, done = std::move(done)](int status, const QJsonValue& response,
                                                         const QString& error) {
            if (attempt != signInEpoch_ || browserSignIn_ != browser) {
                done({}, QStringLiteral("Sign-in was canceled."));
                return;
            }
            const QUrl page(response.toObject().value("authorizationUrl").toString());
            if (status != 200 || !error.isEmpty() || !page.isValid() ||
                page.scheme() != origin_.scheme() || page.host() != origin_.host() ||
                page.port(-1) != origin_.port(-1)) {
                cancelSignIn();
                done({}, error.isEmpty()
                             ? QStringLiteral("Sign-in page was unavailable or invalid.")
                             : error);
                return;
            }
            connect(browser, &daemon::BrowserSignIn::failed, this,
                    [this, attempt](const QString& message) {
                        if (attempt != signInEpoch_)
                            return;
                        cancelSignIn();
                        emit signInFinished(message);
                    });
            connect(
                browser, &daemon::BrowserSignIn::codeReceived, this,
                [this, browser, attempt](const QString& code) {
                    if (attempt != signInEpoch_ || browserSignIn_ != browser)
                        return;
                    const QJsonObject tokenRequest{{"code", code},
                                                   {"codeVerifier", browser->codeVerifier()},
                                                   {"redirectUri", browser->redirectUri()},
                                                   {"installationId", installationId_}};
                    request(
                        "POST", "/v2/auth/token", tokenRequest, {},
                        [this, attempt](int status, const QJsonValue& value, const QString& error) {
                            if (attempt != signInEpoch_)
                                return;
                            QString failure = error;
                            if (failure.isEmpty() && status != 200)
                                failure = value.toObject().value("code").toString(
                                    QStringLiteral("Sign-in was rejected."));
                            if (failure.isEmpty() && !setCredential(value.toObject(), &failure))
                                failure =
                                    failure.isEmpty()
                                        ? QStringLiteral("Could not store the account session.")
                                        : failure;
                            cancelSignIn();
                            emit signInFinished(failure);
                        });
                });
            done(QJsonObject{{"authorizationUrl", page.toString(QUrl::FullyEncoded)}}, {});
        });
}
void ControlV2Client::accessToken(std::function<void(const QString&, const QString&)> done) {
    if (credential_.isEmpty()) {
        done({}, "Sign in before connecting to a remote host.");
        return;
    }
    if (credentialUnsaved_ &&
        credentials_.write("user-session",
                           QJsonDocument(credential_).toJson(QJsonDocument::Compact))) {
        credentialUnsaved_ = false;
        credentials_.remove("refresh-operation");
    }
    if (QDateTime::fromString(credential_.value("expiresAt").toString(), Qt::ISODateWithMs) >
        QDateTime::currentDateTimeUtc().addSecs(30)) {
        done(credential_.value("accessToken").toString(), {});
        return;
    }
    refreshWaiters_.append(std::move(done));
    if (refreshing_)
        return;
    refreshing_ = true;
    const auto epoch = epoch_;
    auto operation = QString::fromUtf8(credentials_.read("refresh-operation"));
    if (operation.isEmpty()) {
        operation = QUuid::createUuid().toString(QUuid::Id128);
        if (!credentials_.write("refresh-operation", operation.toUtf8())) {
            refreshing_ = false;
            for (const auto& waiter : std::exchange(refreshWaiters_, {}))
                waiter({}, "Unable to persist refresh identity.");
            return;
        }
    }
    request(
        "POST", "/v2/auth/refresh",
        QJsonObject{{"refreshToken", credential_.value("refreshToken")},
                    {"operationId", operation},
                    {"installationId", installationId_}},
        {}, [this, epoch](int status, const QJsonValue& response, const QString& error) {
            if (epoch != epoch_)
                return;
            QString failure = error;
            if (failure.isEmpty() && (status != 200 || !validCredential(response.toObject())))
                failure = response.toObject().value("code").toString("Credential refresh failed.");
            if (failure.isEmpty() && response.toObject().value("subject").toObject().value("id") !=
                                         credential_.value("subject").toObject().value("id"))
                failure = "Credential subject changed.";
            if (failure.isEmpty()) {
                auto value = withDeadline(response.toObject());
                value.insert("deviceId", deviceId_);
                value.insert("deviceKeyVersion", keyVersion_);
                // The old refresh token may already be spent: this process keeps the rotated
                // one, and the write is retried on the next access. The refresh operation stays
                // until it is stored, so a restart replays the same rotation (#88).
                credential_ = value;
                QString storageError;
                credentialUnsaved_ = !credentials_.write(
                    "user-session", QJsonDocument(value).toJson(QJsonDocument::Compact),
                    &storageError);
                if (credentialUnsaved_)
                    qWarning("Unable to store rotated remote credentials: %s",
                             qUtf8Printable(storageError));
                else
                    credentials_.remove("refresh-operation");
            }
            const bool revoked = status == 401;
            if (revoked) {
                ++epoch_;
                signerVersions_.clear();
                ++pairingEpoch_;
                credential_ = {};
                deviceId_.clear();
                pairingClaims_.clear();
                credentials_.remove("user-session");
            }
            refreshing_ = false;
            const auto waiters = std::exchange(refreshWaiters_, {});
            for (const auto& waiter : waiters)
                waiter(failure.isEmpty() ? credential_.value("accessToken").toString() : QString{},
                       failure);
            if (revoked)
                emit credentialRevoked();
        });
}
void ControlV2Client::signOut(Callback done) {
    cancelSignIn();
    const auto previous = credential_;
    ++epoch_;
    signerVersions_.clear();
    credential_ = {};
    credentialUnsaved_ = false;
    deviceId_.clear();
    pairingClaims_.clear();
    pairingHosts_.clear();
    refreshing_ = false;
    ++pairingEpoch_;
    // Emptied before the credential is removed, as in setCredential: a credential that
    // survives a failed removal must not adopt host keys on the next start (#88).
    QString localError;
    if (!storeHostPins({}, &localError) && localError.isEmpty())
        localError = "Unable to clear host pins.";
    hostPins_ = {};
    hostPinsStored_ = true;
    QString sessionError;
    if (!credentials_.remove("user-session", &sessionError) && localError.isEmpty())
        localError = sessionError.isEmpty() ? "Unable to remove local credentials." : sessionError;
    QString operationError;
    if (!credentials_.remove("refresh-operation", &operationError) && localError.isEmpty())
        localError =
            operationError.isEmpty() ? "Unable to remove local refresh identity." : operationError;
    for (const auto& waiter : std::exchange(refreshWaiters_, {}))
        waiter({}, "Signed out.");
    const auto token = previous.value("accessToken").toString();
    if (token.isEmpty()) {
        done(QJsonObject{{"ok", localError.isEmpty()}}, localError);
        return;
    }
    // Only a 200 revoked the session on the server; a 401 means the access token had expired
    // and the refresh token is still valid there (#88).
    const auto failed = QStringLiteral("Local sign-out completed; remote revocation failed.");
    auto logout = [this, done, localError, failed](const QString& accessToken) {
        request(
            "POST", "/v2/auth/logout", QJsonObject{}, accessToken,
            [done, localError, failed](int status, const QJsonValue& value, const QString& error) {
                done(value, !localError.isEmpty() ? localError : status == 200 ? error : failed);
            });
    };
    if (QDateTime::fromString(previous.value("expiresAt").toString(), Qt::ISODateWithMs) >
        QDateTime::currentDateTimeUtc().addSecs(30)) {
        logout(token);
        return;
    }
    // An expired access token cannot revoke anything: rotate once, then revoke the new session.
    request("POST", "/v2/auth/refresh",
            QJsonObject{{"refreshToken", previous.value("refreshToken")},
                        {"operationId", QUuid::createUuid().toString(QUuid::Id128)},
                        {"installationId", installationId_}},
            {},
            [done, localError, failed, logout = std::move(logout)](
                int status, const QJsonValue& response, const QString& error) {
                if (status == 200 && error.isEmpty() && validCredential(response.toObject())) {
                    logout(response.toObject().value("accessToken").toString());
                    return;
                }
                done(QJsonObject{{"ok", false}}, localError.isEmpty() ? failed : localError);
            });
}
bool ControlV2Client::validatePairing(const QJsonObject& ticket, QString* error) {
    const auto device = ticket.value("claimedDevice").toObject();
    const auto keys = identity(error).value("keys").toObject();
    const auto digest = QString::fromLatin1(
        QCryptographicHash::hash(
            daemon::CloudCrypto::canonical(
                {ticket.value("id"), device.value("id"), device.value("keyVersion"),
                 keys.value("agreementPublicKey"), keys.value("signingPublicKey")}),
            QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const auto previous = pairingClaims_.value(ticket.value("id").toString());
    // The ticket must belong to the host the QR code named.
    const auto host = pairingHosts_.value(ticket.value("id").toString());
    if (keys.isEmpty() ||
        !QStringList{"claimed", "approved"}.contains(ticket.value("status").toString()) ||
        !QStringList{"pending", "active"}.contains(device.value("status").toString()) ||
        ticket.value("machineId").toString().isEmpty() || device.value("id").toString().isEmpty() ||
        device.value("installationId").toString() != installationId_ ||
        device.value("platform").toString() != daemon::hostPlatform() ||
        device.value("keyVersion").toInt() < 1 ||
        ticket.value("claimDigest").toString() != digest ||
        (!host.isEmpty() && ticket.value("machineId").toString() != host.value("machineId")) ||
        (!previous.isEmpty() &&
         (previous.value("machineId") != ticket.value("machineId") ||
          previous.value("claimDigest") != ticket.value("claimDigest") ||
          previous.value("claimedDevice").toObject().value("id") != device.value("id")))) {
        if (error)
            *error = "Pairing claim identity changed.";
        return false;
    }
    return true;
}
void ControlV2Client::claimPairing(const QString& ticketId, const QString& secret,
                                   const QString& displayName, const QJsonObject& host,
                                   Callback done) {
    cancelPairing();
    const auto attempt = pairingEpoch_;
    QString error;
    const auto own = identity(&error);
    if (own.isEmpty() || ticketId.isEmpty() || secret.size() < 32) {
        done({}, error.isEmpty() ? "Invalid pairing code." : error);
        return;
    }
    // Without the host's fingerprint this client could only take the relay's word for the
    // host's keys (#88).
    if (host.value("machineId").toString().isEmpty() || host.value("keyVersion").toInt() < 1 ||
        daemon::CloudCrypto::fromBase64Url(host.value("fingerprint").toString()).size() != 32) {
        done({}, "This pairing code does not identify its host. Update KetPlus on the host and "
                 "create a new code.");
        return;
    }
    pairingHosts_.insert(ticketId, host);
    authorized("POST",
               "/v2/mobile/pairing/tickets/" +
                   QString::fromUtf8(QUrl::toPercentEncoding(ticketId)) + "/claim",
               QJsonObject{{"secret", secret},
                           {"installationId", installationId_},
                           {"keys", own.value("keys")},
                           {"displayName", displayName.left(128)},
                           {"platform", daemon::hostPlatform()}},
               [this, ticketId, attempt, done = std::move(done)](const QJsonValue& value,
                                                                 const QString& failure) {
                   if (attempt != pairingEpoch_) {
                       done({}, "Pairing was canceled.");
                       return;
                   }
                   QString error = failure;
                   const auto ticket = value.toObject();
                   if (error.isEmpty() && (ticket.value("id").toString() != ticketId ||
                                           ticket.value("status").toString() != "claimed" ||
                                           !validatePairing(ticket, &error)))
                       error = error.isEmpty() ? "Invalid pairing ticket." : error;
                   if (error.isEmpty())
                       pairingClaims_.insert(ticketId, ticket);
                   done(value, error);
               });
}
void ControlV2Client::pairingStatus(const QString& ticketId, Callback done) {
    if (!pairingClaims_.contains(ticketId)) {
        done({}, "No current pairing attempt.");
        return;
    }
    const auto attempt = pairingEpoch_;
    authorized("GET",
               "/v2/mobile/pairing/tickets/" + QString::fromUtf8(QUrl::toPercentEncoding(ticketId)),
               {},
               [this, ticketId, attempt, done = std::move(done)](const QJsonValue& value,
                                                                 const QString& failure) {
                   if (attempt != pairingEpoch_) {
                       done({}, "Pairing was canceled.");
                       return;
                   }
                   QString error = failure;
                   if (error.isEmpty() && (value.toObject().value("id").toString() != ticketId ||
                                           !validatePairing(value.toObject(), &error)))
                       error = "Invalid pairing ticket.";
                   done(value, error);
               });
}
void ControlV2Client::cancelPairing() {
    ++pairingEpoch_;
    pairingClaims_.clear();
    pairingHosts_.clear();
}
bool ControlV2Client::acceptPairing(const QJsonObject& ticket, QString* error) {
    if (!pairingClaims_.contains(ticket.value("id").toString()) ||
        ticket.value("status").toString() != "approved" || !validatePairing(ticket, error)) {
        if (error && error->isEmpty())
            *error = "Pairing is not approved for the current attempt.";
        return false;
    }
    const auto host = pairingHosts_.value(ticket.value("id").toString());
    if (host.isEmpty()) {
        if (error)
            *error = "Pairing is not approved for the current attempt.";
        return false;
    }
    auto value = credential_;
    const auto device = ticket.value("claimedDevice").toObject();
    value.insert("deviceId", device.value("id"));
    value.insert("deviceKeyVersion", device.value("keyVersion"));
    // "machine/version" -> key fingerprint: senderKey accepts only these host keys (#88).
    auto pins = hostPins_;
    pins.insert(QStringLiteral("%1/%2")
                    .arg(host.value("machineId").toString())
                    .arg(host.value("keyVersion").toInt()),
                host.value("fingerprint"));
    if (!storeHostPins(pins, error))
        return false;
    if (!credentials_.write("user-session", QJsonDocument(value).toJson(QJsonDocument::Compact),
                            error))
        return false;
    credential_ = value;
    deviceId_ = device.value("id").toString();
    keyVersion_ = device.value("keyVersion").toInt();
    pairingClaims_.clear();
    pairingHosts_.clear();
    return true;
}
} // namespace ketplus::remote
