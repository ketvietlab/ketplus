#pragma once
#include "cloud/CloudCrypto.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

// Shared by the ControlV2Client translation units: structural checks for host-signed session
// content before it is handed to CloudCrypto.
namespace ketplus::remote::control_v2 {
// sessionKeys() reports this when the host has not wrapped the key for this device yet.
inline constexpr char noSessionKeyGrant[] =
    "No host-signed session key was granted to this device.";
// senderKey() reports this when the relay serves host keys that no pin names.
inline constexpr char hostNotPaired[] = "This host is not paired with this computer.";
inline QString segment(const QString& value) {
    return QString::fromUtf8(QUrl::toPercentEncoding(value));
}
inline bool validContent(const QJsonObject& content, const QString& session, qint64 generation,
                         qsizetype maximum) {
    const auto bytes = [&](const char* field) {
        return daemon::CloudCrypto::fromBase64Url(content.value(QLatin1String(field)).toString());
    };
    return content.value("version").toInt() == 1 &&
           content.value("algorithm").toString() == "A256GCM" &&
           content.value("keyEpoch").toInt() == 1 &&
           content.value("keyId").toString() == QString("%1:%2:1").arg(session).arg(generation) &&
           content.value("senderKeyVersion").toInt() > 0 && bytes("nonce").size() == 12 &&
           bytes("tag").size() == 16 && bytes("signature").size() == 64 &&
           bytes("ciphertext").size() <= maximum;
}
inline QJsonArray contentScope(const QJsonObject& content) {
    return {content.value("keyId"), content.value("keyEpoch"), content.value("senderDeviceId"),
            content.value("senderKeyVersion")};
}
} // namespace ketplus::remote::control_v2
