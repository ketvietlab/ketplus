#include "cloud/DevicePins.h"

#include "cloud/CloudCrypto.h"
#include "cloud/CredentialStore.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace ketplus::daemon {
namespace {

const QString account = QStringLiteral("device-pins");

bool active(const QJsonObject& device) {
    return !device.contains(QStringLiteral("status")) ||
           device.value(QStringLiteral("status")) == QStringLiteral("active");
}

QString pinKey(const QString& deviceId, const int keyVersion) {
    return QStringLiteral("%1/%2").arg(deviceId).arg(keyVersion);
}

// Both public keys, exactly as the relay presented them at approval.
QString fingerprint(const QJsonObject& keys) {
    const QString agreement = keys.value(QStringLiteral("agreementPublicKey")).toString();
    const QString signing = keys.value(QStringLiteral("signingPublicKey")).toString();
    if (CloudCrypto::fromBase64Url(agreement).size() != 32 ||
        CloudCrypto::fromBase64Url(signing).size() != 32)
        return {};
    return QString::fromLatin1(CloudCrypto::base64Url(QCryptographicHash::hash(
        CloudCrypto::canonical({agreement, signing}), QCryptographicHash::Sha256)));
}

} // namespace

DevicePins::DevicePins(CredentialStore* store) : store_(store) {}

QString DevicePins::claimDigest(const QString& ticketId, const QJsonObject& device) {
    const QJsonObject keys = device.value(QStringLiteral("keys")).toObject();
    return QString::fromLatin1(CloudCrypto::base64Url(QCryptographicHash::hash(
        CloudCrypto::canonical({ticketId, device.value(QStringLiteral("id")),
                                device.value(QStringLiteral("keyVersion")),
                                keys.value(QStringLiteral("agreementPublicKey")),
                                keys.value(QStringLiteral("signingPublicKey"))}),
        QCryptographicHash::Sha256)));
}

QString DevicePins::confirmationCode(const QString& claimDigest) {
    QString code = claimDigest.toUpper();
    code.remove(QLatin1Char('-'));
    code.remove(QLatin1Char(' '));
    return code.size() < 6 ? QString{} : code.left(6);
}

QString DevicePins::hostFingerprint(const QJsonObject& keys) {
    const QByteArray signing =
        CloudCrypto::fromBase64Url(keys.value(QStringLiteral("signingPublicKey")).toString());
    const QByteArray agreement =
        CloudCrypto::fromBase64Url(keys.value(QStringLiteral("agreementPublicKey")).toString());
    if (signing.size() != 32 || agreement.size() != 32)
        return {};
    return QString::fromLatin1(CloudCrypto::base64Url(
        QCryptographicHash::hash(signing + agreement, QCryptographicHash::Sha256)));
}

QJsonObject DevicePins::read(QString* error) const {
    QString readError;
    const QByteArray bytes = store_->read(account, &readError);
    if (error != nullptr)
        *error = readError;
    return QJsonDocument::fromJson(bytes).object();
}

bool DevicePins::write(const QJsonObject& pins, QString* error) const {
    return store_->write(account, QJsonDocument(pins).toJson(QJsonDocument::Compact), error);
}

bool DevicePins::initialized(QString* error) const {
    QString readError;
    const QJsonObject pins = read(&readError);
    if (error != nullptr)
        *error = readError;
    return readError.isEmpty() && pins.value(QStringLiteral("version")).toInt() == 1;
}

bool DevicePins::adopt(const QJsonArray& devices, QString* error) const {
    QString readError;
    if (initialized(&readError) || !readError.isEmpty()) {
        if (error != nullptr)
            *error = readError;
        return readError.isEmpty();
    }
    QJsonObject pinned;
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        const QString id = device.value(QStringLiteral("id")).toString();
        const QString print = fingerprint(device.value(QStringLiteral("keys")).toObject());
        if (active(device) && !id.isEmpty() && !print.isEmpty())
            pinned.insert(pinKey(id, device.value(QStringLiteral("keyVersion")).toInt()), print);
    }
    return write({{QStringLiteral("version"), 1}, {QStringLiteral("devices"), pinned}}, error);
}

bool DevicePins::reset(QString* error) const {
    return write({{QStringLiteral("version"), 1}, {QStringLiteral("devices"), QJsonObject{}}},
                 error);
}

bool DevicePins::pin(const QJsonObject& device, QString* error) const {
    const QString id = device.value(QStringLiteral("id")).toString();
    const QString print = fingerprint(device.value(QStringLiteral("keys")).toObject());
    if (id.isEmpty() || print.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("The paired device has no valid keys.");
        return false;
    }
    QString readError;
    QJsonObject pins = read(&readError);
    if (!readError.isEmpty()) {
        if (error != nullptr)
            *error = readError;
        return false;
    }
    QJsonObject devices = pins.value(QStringLiteral("devices")).toObject();
    devices.insert(pinKey(id, device.value(QStringLiteral("keyVersion")).toInt()), print);
    pins.insert(QStringLiteral("version"), 1);
    pins.insert(QStringLiteral("devices"), devices);
    return write(pins, error);
}

bool DevicePins::unpin(const QString& deviceId, QString* error) const {
    QString readError;
    QJsonObject pins = read(&readError);
    if (!readError.isEmpty()) {
        if (error != nullptr)
            *error = readError;
        return false;
    }
    QJsonObject devices = pins.value(QStringLiteral("devices")).toObject();
    const QString prefix = deviceId + QLatin1Char('/');
    for (const QString& key : devices.keys()) {
        if (key.startsWith(prefix))
            devices.remove(key);
    }
    pins.insert(QStringLiteral("version"), 1);
    pins.insert(QStringLiteral("devices"), devices);
    return write(pins, error);
}

bool DevicePins::trustsKeys(const QString& deviceId, const int keyVersion,
                            const QJsonObject& keys) const {
    const QString print = fingerprint(keys);
    if (deviceId.isEmpty() || print.isEmpty())
        return false;
    QString readError;
    const QJsonObject devices = read(&readError).value(QStringLiteral("devices")).toObject();
    return readError.isEmpty() && devices.value(pinKey(deviceId, keyVersion)).toString() == print;
}

bool DevicePins::trusts(const QJsonObject& device) const {
    return active(device) && trustsKeys(device.value(QStringLiteral("id")).toString(),
                                        device.value(QStringLiteral("keyVersion")).toInt(),
                                        device.value(QStringLiteral("keys")).toObject());
}

QJsonArray DevicePins::trusted(const QJsonArray& devices) const {
    QJsonArray result;
    for (const QJsonValue& value : devices) {
        if (trusts(value.toObject()))
            result.append(value);
    }
    return result;
}

} // namespace ketplus::daemon
