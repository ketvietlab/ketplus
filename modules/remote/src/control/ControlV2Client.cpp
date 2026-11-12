#include "control/ControlV2Client.h"
#include "cloud/ControlEndpoint.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUuid>
namespace ketplus::remote {
namespace {
QString storageNamespace(const QUrl& origin) {
    return "dev.ketplus.remote.control." +
           QString::fromLatin1(
               QCryptographicHash::hash(origin.toEncoded(), QCryptographicHash::Sha256)
                   .toHex()
                   .left(24));
}
bool allowedOrigin(const QUrl& origin) {
    return origin.isValid() && !origin.host().isEmpty() && origin.userInfo().isEmpty() &&
           !origin.hasQuery() && !origin.hasFragment() &&
           (origin.scheme() == "https" ||
            (origin.scheme() == "http" &&
             (origin.host() == "127.0.0.1" || origin.host() == "localhost")));
}
} // namespace
ControlV2Client::ControlV2Client(QUrl origin, QObject* parent)
    : QObject(parent), origin_(std::move(origin)), credentials_(storageNamespace(origin_)) {
    installationId_ = QString::fromUtf8(credentials_.read("installation-id"));
    credential_ = QJsonDocument::fromJson(credentials_.read("user-session")).object();
    deviceId_ = credential_.value("deviceId").toString();
    keyVersion_ = credential_.value("deviceKeyVersion").toInt(1);
    const QByteArray pins = credentials_.read("host-pins");
    hostPinsStored_ = !pins.isEmpty();
    hostPins_ = QJsonDocument::fromJson(pins).object();
}
bool ControlV2Client::storeHostPins(const QJsonObject& pins, QString* error) {
    if (!credentials_.write("host-pins", QJsonDocument(pins).toJson(QJsonDocument::Compact), error))
        return false;
    hostPins_ = pins;
    hostPinsStored_ = true;
    return true;
}
QString ControlV2Client::pinnedFingerprint(const QString& machineId) const {
    // Pins are "machine/version" -> fingerprint; the machine id itself may contain '/'.
    QString fingerprint;
    int newest = -1;
    for (auto it = hostPins_.constBegin(); it != hostPins_.constEnd(); ++it) {
        const qsizetype slash = it.key().lastIndexOf(QLatin1Char('/'));
        if (slash < 0 || it.key().left(slash) != machineId)
            continue;
        bool ok = false;
        const int version = it.key().mid(slash + 1).toInt(&ok);
        if (ok && version > newest) {
            newest = version;
            fingerprint = it.value().toString();
        }
    }
    return fingerprint;
}
QJsonObject ControlV2Client::identity(QString* error) {
    if (installationId_.isEmpty()) {
        installationId_ = "desktop_" + QUuid::createUuid().toString(QUuid::Id128);
        if (!credentials_.write("installation-id", installationId_.toUtf8(), error)) {
            installationId_.clear();
            return {};
        }
    }
    const auto keys = crypto_.publicKeys(error);
    return keys.isEmpty() ? QJsonObject{}
                          : QJsonObject{{"installationId", installationId_}, {"keys", keys}};
}
void ControlV2Client::request(const QByteArray& method, const QString& path, const QJsonValue& body,
                              const QString& bearer, Reply done) {
    if (!allowedOrigin(origin_)) {
        done(0, {}, "Control origin must be HTTPS or loopback HTTP.");
        return;
    }
    const auto epoch = epoch_;
    QNetworkRequest request(daemon::controlEndpoint(origin_, path));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("X-KetPlus-Control-Version", "2");
    if (!bearer.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    const auto encoded = body.isObject()
                             ? QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact)
                             : QByteArray{};
    auto* reply = transport_->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, epoch, done = std::move(done)] {
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->isOpen() ? reply->read(1024 * 1024 + 1) : QByteArray{};
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(bytes, &parse);
        const QJsonValue body =
            json.isObject() ? QJsonValue(json.object()) : QJsonValue(json.array());
        QString failure;
        if (epoch != epoch_)
            failure = "Account changed while the request was pending.";
        else if (bytes.size() > 1024 * 1024 || parse.error != QJsonParseError::NoError)
            failure = "Invalid or oversized Control response.";
        else if (status == 0 || (reply->error() > QNetworkReply::NoError && reply->error() < 100))
            failure = "Control transport failed.";
        reply->deleteLater();
        done(status, failure.isEmpty() ? body : QJsonValue{}, failure);
    });
}
void ControlV2Client::authorized(const QByteArray& method, const QString& path,
                                 const QJsonValue& body, Callback done) {
    const auto epoch = epoch_;
    accessToken([this, epoch, method, path, body, done = std::move(done)](const QString& token,
                                                                          const QString& error) {
        if (epoch != epoch_ || !error.isEmpty()) {
            done({}, error.isEmpty() ? "Account changed." : error);
            return;
        }
        request(
            method, path, body, token,
            [done = std::move(done)](int status, const QJsonValue& value, const QString& error) {
                done(value,
                     !error.isEmpty() ? error
                     : status >= 200 && status < 300
                         ? QString{}
                         : value.toObject().value("code").toString("Control refused the request."));
            });
    });
}
void ControlV2Client::listSessions(Callback done) {
    authorized("GET", "/v2/mobile/sessions", {}, std::move(done));
}
void ControlV2Client::listWorktrees(Callback done) {
    authorized("GET", "/v2/mobile/worktrees", {}, std::move(done));
}
} // namespace ketplus::remote
