#include "pairing/PairingIdentity.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingToken.h"

#include <QCryptographicHash>
#include <QJsonArray>

#include <openssl/crypto.h>

namespace ketplus::runner::pairing {
namespace cc = ketplus::control_crypto;

QString runnerFingerprint(const QByteArray& runnerPublicKey) {
    if (runnerPublicKey.size() != publicKeyBytes)
        return {};
    return QString::fromLatin1(
        cc::base64Url(QCryptographicHash::hash(runnerPublicKey, QCryptographicHash::Sha256)));
}

QString desktopFingerprint(const QByteArray& signingPublicKey,
                           const QByteArray& agreementPublicKey) {
    return cc::keyFingerprint(signingPublicKey, agreementPublicKey);
}

bool constantTimeEquals(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size())
        return false;
    return left.isEmpty() || CRYPTO_memcmp(left.constData(), right.constData(),
                                           static_cast<size_t>(left.size())) == 0;
}

std::optional<QByteArray> decodeBase64Url(const QString& text) {
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        const bool allowed = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') ||
                             (u >= u'0' && u <= u'9') || u == u'-' || u == u'_';
        if (!allowed)
            return std::nullopt;
    }
    if (text.size() % 4 == 1)
        return std::nullopt;
    const QByteArray value = cc::fromBase64Url(text);
    // Only the canonical spelling: no unused bits set in the last character.
    if (cc::base64Url(value) != text.toLatin1())
        return std::nullopt;
    return value;
}

QByteArray pairingDigestBytes(const PairingDigestInput& input) {
    return cc::canonical({QStringLiteral("ketplus"), 2, QStringLiteral("runner-pair"),
                          input.ticketId, input.workspaceId, input.machineId,
                          input.machineKeyVersion, input.desktopFingerprint, input.runnerId,
                          input.runnerKeyVersion, input.runnerFingerprint});
}

PairingCode pairingCode(const PairingDigestInput& input) {
    const QByteArray hash =
        QCryptographicHash::hash(pairingDigestBytes(input), QCryptographicHash::Sha256);
    // RFC 4648 base32 of the hash, first ten characters (50 bits).
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    QString code;
    quint64 bits = 0;
    int available = 0;
    qsizetype next = 0;
    while (code.size() < 10) {
        if (available < 5) {
            bits = (bits << 8) | static_cast<quint8>(hash.at(next++));
            available += 8;
        }
        available -= 5;
        code += QLatin1Char(alphabet[(bits >> available) & 0x1f]);
    }
    return {QString::fromLatin1(cc::base64Url(hash)),
            code.left(5) + QLatin1Char('-') + code.mid(5)};
}

std::optional<PairingError> checkDesktopKeys(const PairingToken& token,
                                             const ReportedDesktop& desktop) {
    const PairingTokenFields& fields = token.fields();
    const QString fingerprint =
        desktopFingerprint(desktop.signingPublicKey, desktop.agreementPublicKey);
    // Every check runs, so the time taken does not tell which one failed.
    bool matches = !fingerprint.isEmpty();
    matches &= constantTimeEquals(fingerprint.toLatin1(), fields.desktopFingerprint.toLatin1());
    matches &= desktop.machineId == fields.machineId;
    matches &= desktop.machineKeyVersion == fields.machineKeyVersion;
    if (!matches)
        return PairingError::DesktopKeyMismatch;
    return std::nullopt;
}

} // namespace ketplus::runner::pairing
