#include "cloud/CryptoPrimitives.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include <openssl/err.h>
#include <openssl/evp.h>

#include <memory>

namespace ketplus::control_crypto {
namespace {
using PKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PKeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

PKey rawPrivate(const QByteArray& raw, const int type) {
    return PKey(EVP_PKEY_new_raw_private_key(
                    type, nullptr, reinterpret_cast<const unsigned char*>(raw.constData()),
                    static_cast<size_t>(raw.size())),
                EVP_PKEY_free);
}

PKey rawPublic(const QByteArray& raw, const int type) {
    return PKey(EVP_PKEY_new_raw_public_key(type, nullptr,
                                            reinterpret_cast<const unsigned char*>(raw.constData()),
                                            static_cast<size_t>(raw.size())),
                EVP_PKEY_free);
}
} // namespace

QString opensslError(const QString& fallback) {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return fallback;
    }
    char buffer[256]{};
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return QStringLiteral("%1 (%2)").arg(fallback, QString::fromLatin1(buffer));
}

QByteArray base64Url(const QByteArray& value) {
    return value.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray fromBase64Url(const QString& value) {
    return QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding);
}

QByteArray canonical(const QJsonArray& values) {
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}

QString keyFingerprint(const QByteArray& signingPublicKey, const QByteArray& agreementPublicKey) {
    if (signingPublicKey.size() != 32 || agreementPublicKey.size() != 32)
        return {};
    return QString::fromLatin1(base64Url(QCryptographicHash::hash(
        signingPublicKey + agreementPublicKey, QCryptographicHash::Sha256)));
}

QString keyFingerprint(const QJsonObject& keys) {
    return keyFingerprint(
        fromBase64Url(keys.value(QStringLiteral("signingPublicKey")).toString()),
        fromBase64Url(keys.value(QStringLiteral("agreementPublicKey")).toString()));
}

QByteArray generatePrivateKey(const int keyType, QString* error) {
    PKeyContext context(EVP_PKEY_CTX_new_id(keyType, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* generated = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_keygen(context.get(), &generated) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to generate device key."));
        return {};
    }
    PKey key(generated, EVP_PKEY_free);
    size_t size = 0;
    if (EVP_PKEY_get_raw_private_key(key.get(), nullptr, &size) != 1)
        return {};
    QByteArray raw(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (EVP_PKEY_get_raw_private_key(key.get(), reinterpret_cast<unsigned char*>(raw.data()),
                                     &size) != 1)
        return {};
    return raw;
}

QByteArray publicKey(const QByteArray& privateKey, const int keyType, QString* error) {
    PKey key = rawPrivate(privateKey, keyType);
    size_t size = 0;
    if (!key || EVP_PKEY_get_raw_public_key(key.get(), nullptr, &size) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to derive public key."));
        return {};
    }
    QByteArray result(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(result.data()),
                                    &size) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to derive public key."));
        return {};
    }
    return result;
}

Ed25519KeyPair generateEd25519(QString* error) {
    Ed25519KeyPair pair;
    pair.privateKey = generatePrivateKey(EVP_PKEY_ED25519, error);
    if (pair.privateKey.isEmpty())
        return {};
    pair.publicKey = publicKey(pair.privateKey, EVP_PKEY_ED25519, error);
    if (pair.publicKey.size() != 32)
        return {};
    return pair;
}

QByteArray signEd25519(const QByteArray& privateKey, const QByteArray& message, QString* error) {
    PKey key = rawPrivate(privateKey, EVP_PKEY_ED25519);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    size_t size = 64;
    QByteArray signature(64, Qt::Uninitialized);
    if (!key || !context ||
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()), &size,
                       reinterpret_cast<const unsigned char*>(message.constData()),
                       static_cast<size_t>(message.size())) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to sign content."));
        return {};
    }
    signature.resize(static_cast<qsizetype>(size));
    return signature;
}

bool verifyEd25519(const QByteArray& publicKey, const QByteArray& message,
                   const QByteArray& signature) {
    if (publicKey.size() != 32 || signature.size() != 64)
        return false;
    PKey key = rawPublic(publicKey, EVP_PKEY_ED25519);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return key && context &&
           EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
           EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signature.constData()),
                            static_cast<size_t>(signature.size()),
                            reinterpret_cast<const unsigned char*>(message.constData()),
                            static_cast<size_t>(message.size())) == 1;
}

} // namespace ketplus::control_crypto
