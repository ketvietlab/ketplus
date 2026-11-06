#include "cloud/MachineChallenge.h"
#include "cloud/CloudCrypto.h"

#include <QJsonArray>

namespace ketplus::daemon::machine_challenge {
namespace {
bool validId(const QString& value) {
    if (value.isEmpty() || value.size() > 128)
        return false;
    for (const QChar character : value) {
        const auto c = character.unicode();
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}
} // namespace

QByteArray signingBytes(const QJsonObject& challenge, const QString& machineId,
                        const int keyVersion) {
    const QString id = challenge.value(QStringLiteral("id")).toString();
    const QJsonValue version = challenge.value(QStringLiteral("signatureVersion"));
    const QJsonValue key = challenge.value(QStringLiteral("keyVersion"));
    if (!validId(id) || !validId(machineId) || keyVersion < 1 || !version.isDouble() ||
        version.toDouble() != 2 ||
        challenge.value(QStringLiteral("signatureAlgorithm")) != QStringLiteral("Ed25519") ||
        challenge.value(QStringLiteral("machineId")) != machineId || !key.isDouble() ||
        key.toDouble() != keyVersion)
        return {};

    const QString encoded = challenge.value(QStringLiteral("challenge")).toString();
    // 32 bytes in unpadded base64url occupy exactly 43 ASCII characters. Checking the
    // round trip also rejects padding, noncanonical trailing bits and permissive decodes.
    if (encoded.size() != 43)
        return {};
    const QByteArray nonce = CloudCrypto::fromBase64Url(encoded);
    if (nonce.size() != 32 || QString::fromLatin1(CloudCrypto::base64Url(nonce)) != encoded)
        return {};

    QByteArray bytes =
        CloudCrypto::canonical({QStringLiteral("ketplus"), 2, QStringLiteral("daemon-challenge"),
                                id, machineId, keyVersion});
    bytes.append('\0');
    bytes.append(nonce);
    return bytes;
}
} // namespace ketplus::daemon::machine_challenge
