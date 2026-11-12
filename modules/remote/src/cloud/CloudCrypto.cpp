#include "cloud/CloudCrypto.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <memory>

namespace ketplus::daemon {
namespace {

using PKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PKeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

QString opensslError(const QString& fallback) {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return fallback;
    }
    char buffer[256]{};
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return QStringLiteral("%1 (%2)").arg(fallback, QString::fromLatin1(buffer));
}

QByteArray randomBytes(const int size, QString* error) {
    QByteArray bytes(size, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), size) != 1) {
        if (error != nullptr) {
            *error = opensslError(QStringLiteral("Unable to generate secure random bytes."));
        }
        return {};
    }
    return bytes;
}

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

QByteArray aesGcmEncrypt(const QByteArray& plaintext, const QByteArray& aad, const QByteArray& key,
                         const QByteArray& nonce, QByteArray* tag, QString* error) {
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(key.constData()),
                           reinterpret_cast<const unsigned char*>(nonce.constData())) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to initialize encryption."));
        return {};
    }
    int written = 0;
    if (!aad.isEmpty() && EVP_EncryptUpdate(context.get(), nullptr, &written,
                                            reinterpret_cast<const unsigned char*>(aad.constData()),
                                            aad.size()) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to authenticate metadata."));
        return {};
    }
    QByteArray ciphertext(plaintext.size() + 16, Qt::Uninitialized);
    int total = 0;
    if (EVP_EncryptUpdate(context.get(), reinterpret_cast<unsigned char*>(ciphertext.data()),
                          &written, reinterpret_cast<const unsigned char*>(plaintext.constData()),
                          plaintext.size()) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to encrypt content."));
        return {};
    }
    total += written;
    if (EVP_EncryptFinal_ex(context.get(),
                            reinterpret_cast<unsigned char*>(ciphertext.data()) + total,
                            &written) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to finalize encryption."));
        return {};
    }
    total += written;
    tag->resize(16);
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag->size(), tag->data()) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to read authentication tag."));
        return {};
    }
    ciphertext.resize(total);
    return ciphertext;
}

QByteArray aesGcmDecrypt(const QByteArray& ciphertext, const QByteArray& aad, const QByteArray& key,
                         const QByteArray& nonce, const QByteArray& tag, QString* error) {
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(key.constData()),
                           reinterpret_cast<const unsigned char*>(nonce.constData())) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to initialize decryption."));
        return {};
    }
    int written = 0;
    if (!aad.isEmpty() && EVP_DecryptUpdate(context.get(), nullptr, &written,
                                            reinterpret_cast<const unsigned char*>(aad.constData()),
                                            aad.size()) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to authenticate metadata."));
        return {};
    }
    QByteArray plaintext(ciphertext.size() + 16, Qt::Uninitialized);
    int total = 0;
    if (EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plaintext.data()),
                          &written, reinterpret_cast<const unsigned char*>(ciphertext.constData()),
                          ciphertext.size()) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to decrypt content."));
        return {};
    }
    total += written;
    QByteArray mutableTag = tag;
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, mutableTag.size(),
                            mutableTag.data()) != 1 ||
        EVP_DecryptFinal_ex(context.get(),
                            reinterpret_cast<unsigned char*>(plaintext.data()) + total,
                            &written) != 1) {
        if (error != nullptr)
            *error = QStringLiteral("Encrypted content authentication failed.");
        return {};
    }
    total += written;
    plaintext.resize(total);
    return plaintext;
}

} // namespace

CloudCrypto::CloudCrypto(CredentialStore* store) : store_(store) {}

QByteArray CloudCrypto::base64Url(const QByteArray& value) {
    return value.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray CloudCrypto::fromBase64Url(const QString& value) {
    return QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding);
}

QString CloudCrypto::keyFingerprint(const QJsonObject& keys) {
    const QByteArray signing =
        fromBase64Url(keys.value(QStringLiteral("signingPublicKey")).toString());
    const QByteArray agreement =
        fromBase64Url(keys.value(QStringLiteral("agreementPublicKey")).toString());
    if (signing.size() != 32 || agreement.size() != 32)
        return {};
    return QString::fromLatin1(
        base64Url(QCryptographicHash::hash(signing + agreement, QCryptographicHash::Sha256)));
}

QByteArray CloudCrypto::canonical(const QJsonArray& values) {
    return QJsonDocument(values).toJson(QJsonDocument::Compact);
}

QByteArray CloudCrypto::privateKey(const QString& account, const int keyType, QString* error) {
    QString readError;
    QByteArray held = store_->read(account, &readError);
    if (!readError.isEmpty()) {
        // An unreadable store is not a missing key: replacing the identity would break pairing.
        if (error != nullptr)
            *error = readError;
        return {};
    }
    if (!held.isEmpty())
        return held;
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
    held.resize(static_cast<qsizetype>(size));
    if (EVP_PKEY_get_raw_private_key(key.get(), reinterpret_cast<unsigned char*>(held.data()),
                                     &size) != 1 ||
        !store_->write(account, held, error)) {
        return {};
    }
    return held;
}

QByteArray CloudCrypto::publicKey(const QByteArray& privateBytes, const int keyType,
                                  QString* error) {
    PKey key = rawPrivate(privateBytes, keyType);
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

QJsonObject CloudCrypto::publicKeys(QString* error) {
    const QByteArray agreement =
        privateKey(QStringLiteral("agreement-private-key"), EVP_PKEY_X25519, error);
    const QByteArray signing =
        privateKey(QStringLiteral("signing-private-key"), EVP_PKEY_ED25519, error);
    const QByteArray agreementPublic = publicKey(agreement, EVP_PKEY_X25519, error);
    const QByteArray signingPublic = publicKey(signing, EVP_PKEY_ED25519, error);
    if (agreementPublic.size() != 32 || signingPublic.size() != 32)
        return {};
    return {{QStringLiteral("agreementAlgorithm"), QStringLiteral("X25519")},
            {QStringLiteral("agreementPublicKey"), QString::fromLatin1(base64Url(agreementPublic))},
            {QStringLiteral("signingAlgorithm"), QStringLiteral("Ed25519")},
            {QStringLiteral("signingPublicKey"), QString::fromLatin1(base64Url(signingPublic))}};
}

QByteArray CloudCrypto::sign(const QByteArray& bytes, QString* error) {
    const QByteArray raw =
        privateKey(QStringLiteral("signing-private-key"), EVP_PKEY_ED25519, error);
    PKey key = rawPrivate(raw, EVP_PKEY_ED25519);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    size_t size = 64;
    QByteArray signature(64, Qt::Uninitialized);
    if (!key || !context ||
        EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()), &size,
                       reinterpret_cast<const unsigned char*>(bytes.constData()),
                       bytes.size()) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to sign content."));
        return {};
    }
    signature.resize(static_cast<qsizetype>(size));
    return signature;
}

QString CloudCrypto::sessionKeyAccount(const QString& sessionId, const qint64 generation,
                                       const int keyEpoch) {
    const QByteArray digest =
        QCryptographicHash::hash(sessionId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("session-key-%1-%2-%3")
        .arg(QString::fromLatin1(digest), QString::number(generation), QString::number(keyEpoch));
}

QByteArray CloudCrypto::sessionKey(const QString& sessionId, const qint64 generation,
                                   QString* error) {
    const QString account = sessionKeyAccount(sessionId, generation, 1);
    QString readError;
    QByteArray key = store_->read(account, &readError);
    if (!readError.isEmpty()) {
        // Regenerating would make earlier ciphertext for this session undecryptable.
        if (error != nullptr)
            *error = readError;
        return {};
    }
    if (!key.isEmpty())
        return key;
    key = randomBytes(32, error);
    return key.size() == 32 && store_->write(account, key, error) ? key : QByteArray{};
}

QJsonObject CloudCrypto::encrypt(const QByteArray& plaintext, const QByteArray& associatedData,
                                 const QString& sessionId, const qint64 generation,
                                 const int keyEpoch, const QString& senderDeviceId,
                                 const int senderKeyVersion, QString* error) {
    // Only epoch 1 exists: an envelope labelled with another epoch would name a key it was not
    // sealed with (#84).
    if (keyEpoch != 1) {
        if (error != nullptr)
            *error = QStringLiteral("Session key epoch %1 does not exist.").arg(keyEpoch);
        return {};
    }
    const QByteArray key = sessionKey(sessionId, generation, error);
    const QByteArray nonce = randomBytes(12, error);
    QByteArray tag;
    const QByteArray ciphertext = aesGcmEncrypt(plaintext, associatedData, key, nonce, &tag, error);
    if (key.size() != 32 || nonce.size() != 12 || tag.size() != 16)
        return {};
    QByteArray signingInput = associatedData;
    signingInput.append('\0');
    signingInput.append(nonce);
    signingInput.append(ciphertext);
    signingInput.append(tag);
    const QByteArray signature = sign(signingInput, error);
    if (signature.size() != 64)
        return {};
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("algorithm"), QStringLiteral("A256GCM")},
            {QStringLiteral("keyId"),
             QStringLiteral("%1:%2:%3").arg(sessionId).arg(generation).arg(keyEpoch)},
            {QStringLiteral("keyEpoch"), keyEpoch},
            {QStringLiteral("senderDeviceId"), senderDeviceId},
            {QStringLiteral("senderKeyVersion"), senderKeyVersion},
            {QStringLiteral("nonce"), QString::fromLatin1(base64Url(nonce))},
            {QStringLiteral("ciphertext"), QString::fromLatin1(base64Url(ciphertext))},
            {QStringLiteral("tag"), QString::fromLatin1(base64Url(tag))},
            {QStringLiteral("signature"), QString::fromLatin1(base64Url(signature))}};
}

QByteArray CloudCrypto::decrypt(const QJsonObject& content, const QByteArray& associatedData,
                                const QByteArray& senderSigningKey, const QString& sessionId,
                                const qint64 generation, QString* error) {
    const QByteArray nonce = fromBase64Url(content.value(QStringLiteral("nonce")).toString());
    const QByteArray ciphertext =
        fromBase64Url(content.value(QStringLiteral("ciphertext")).toString());
    const QByteArray tag = fromBase64Url(content.value(QStringLiteral("tag")).toString());
    const QByteArray signature =
        fromBase64Url(content.value(QStringLiteral("signature")).toString());
    // AES-GCM accepts other nonce lengths and truncated tags; the format allows only these.
    if (nonce.size() != 12 || tag.size() != 16) {
        if (error != nullptr)
            *error = QStringLiteral("The encrypted envelope is malformed.");
        return {};
    }
    PKey key = rawPublic(senderSigningKey, EVP_PKEY_ED25519);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    QByteArray signingInput = associatedData;
    signingInput.append('\0');
    signingInput.append(nonce);
    signingInput.append(ciphertext);
    signingInput.append(tag);
    if (!key || !context || signature.size() != 64 ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
        EVP_DigestVerify(
            context.get(), reinterpret_cast<const unsigned char*>(signature.constData()),
            signature.size(), reinterpret_cast<const unsigned char*>(signingInput.constData()),
            signingInput.size()) != 1) {
        if (error != nullptr)
            *error = QStringLiteral("Remote signature verification failed.");
        return {};
    }
    const int epoch = content.value(QStringLiteral("keyEpoch")).toInt();
    const QByteArray session = store_->read(sessionKeyAccount(sessionId, generation, epoch), error);
    return aesGcmDecrypt(ciphertext, associatedData, session, nonce, tag, error);
}

QByteArray CloudCrypto::decryptSealed(const QJsonObject& content, const QByteArray& associatedData,
                                      const QByteArray& senderSigningKey, const QString& requestId,
                                      QString* error) {
    return decryptSealedAs(content, associatedData, senderSigningKey, requestId,
                           QStringLiteral("session-create:%1").arg(requestId), error);
}

QByteArray CloudCrypto::decryptSealedAs(const QJsonObject& content,
                                        const QByteArray& associatedData,
                                        const QByteArray& senderSigningKey,
                                        const QString& requestId, const QString& keyId,
                                        QString* error) {
    if (content.value(QStringLiteral("version")).toInt() != 1 ||
        content.value(QStringLiteral("algorithm")).toString() != QStringLiteral("A256GCM") ||
        content.value(QStringLiteral("keyEpoch")).toInt() != 1 ||
        content.value(QStringLiteral("keyId")).toString() != keyId) {
        if (error)
            *error = QStringLiteral("The sealed command envelope is invalid.");
        return {};
    }
    const QByteArray nonce = fromBase64Url(content.value(QStringLiteral("nonce")).toString());
    const QByteArray ciphertext =
        fromBase64Url(content.value(QStringLiteral("ciphertext")).toString());
    const QByteArray tag = fromBase64Url(content.value(QStringLiteral("tag")).toString());
    const QByteArray signature =
        fromBase64Url(content.value(QStringLiteral("signature")).toString());
    const QByteArray ephemeral =
        fromBase64Url(content.value(QStringLiteral("ephemeralPublicKey")).toString());
    if (nonce.size() != 12 || tag.size() != 16) {
        if (error)
            *error = QStringLiteral("The sealed command envelope is invalid.");
        return {};
    }
    PKey signing = rawPublic(senderSigningKey, EVP_PKEY_ED25519);
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    QByteArray input = associatedData;
    input.append('\0');
    input.append(nonce);
    input.append(ciphertext);
    input.append(tag);
    if (!signing || !context || ephemeral.size() != 32 || signature.size() != 64 ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, signing.get()) != 1 ||
        EVP_DigestVerify(
            context.get(), reinterpret_cast<const unsigned char*>(signature.constData()),
            signature.size(), reinterpret_cast<const unsigned char*>(input.constData()),
            input.size()) != 1) {
        if (error)
            *error = QStringLiteral("Remote signature verification failed.");
        return {};
    }
    const QByteArray local =
        privateKey(QStringLiteral("agreement-private-key"), EVP_PKEY_X25519, error);
    const QByteArray key = derive(local, ephemeral, requestId.toUtf8(), error);
    return aesGcmDecrypt(ciphertext, associatedData, key, nonce, tag, error);
}

QJsonObject CloudCrypto::encryptSealed(const QByteArray& plaintext,
                                       const QByteArray& associatedData,
                                       const QByteArray& recipientAgreementKey,
                                       const QString& requestId, const QString& recipientDeviceId,
                                       const int recipientKeyVersion, const QString& senderDeviceId,
                                       const int senderKeyVersion, QString* error) {
    return encryptSealedAs(plaintext, associatedData, recipientAgreementKey, requestId,
                           QStringLiteral("session-create:%1").arg(requestId), recipientDeviceId,
                           recipientKeyVersion, senderDeviceId, senderKeyVersion, error);
}

QJsonObject
CloudCrypto::encryptSealedAs(const QByteArray& plaintext, const QByteArray& associatedData,
                             const QByteArray& recipientAgreementKey, const QString& requestId,
                             const QString& keyId, const QString& recipientDeviceId,
                             const int recipientKeyVersion, const QString& senderDeviceId,
                             const int senderKeyVersion, QString* error) {
    PKeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* generated = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_keygen(context.get(), &generated) != 1)
        return {};
    PKey ephemeral(generated, EVP_PKEY_free);
    QByteArray privateBytes(32, Qt::Uninitialized), publicBytes(32, Qt::Uninitialized);
    size_t privateSize = 32, publicSize = 32;
    if (EVP_PKEY_get_raw_private_key(ephemeral.get(),
                                     reinterpret_cast<unsigned char*>(privateBytes.data()),
                                     &privateSize) != 1 ||
        EVP_PKEY_get_raw_public_key(ephemeral.get(),
                                    reinterpret_cast<unsigned char*>(publicBytes.data()),
                                    &publicSize) != 1)
        return {};
    const QByteArray key = derive(privateBytes, recipientAgreementKey, requestId.toUtf8(), error);
    const QByteArray nonce = randomBytes(12, error);
    QByteArray tag;
    const QByteArray ciphertext = aesGcmEncrypt(plaintext, associatedData, key, nonce, &tag, error);
    QByteArray input = associatedData;
    input.append('\0');
    input.append(nonce);
    input.append(ciphertext);
    input.append(tag);
    const QByteArray signature = sign(input, error);
    if (key.size() != 32 || nonce.size() != 12 || tag.size() != 16 || signature.size() != 64)
        return {};
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("algorithm"), QStringLiteral("A256GCM")},
            {QStringLiteral("keyId"), keyId},
            {QStringLiteral("keyEpoch"), 1},
            {QStringLiteral("senderDeviceId"), senderDeviceId},
            {QStringLiteral("senderKeyVersion"), senderKeyVersion},
            {QStringLiteral("recipientDeviceId"), recipientDeviceId},
            {QStringLiteral("recipientKeyVersion"), recipientKeyVersion},
            {QStringLiteral("ephemeralPublicKey"), QString::fromLatin1(base64Url(publicBytes))},
            {QStringLiteral("nonce"), QString::fromLatin1(base64Url(nonce))},
            {QStringLiteral("ciphertext"), QString::fromLatin1(base64Url(ciphertext))},
            {QStringLiteral("tag"), QString::fromLatin1(base64Url(tag))},
            {QStringLiteral("signature"), QString::fromLatin1(base64Url(signature))}};
}

QByteArray CloudCrypto::derive(const QByteArray& privateBytes, const QByteArray& peerPublicKey,
                               const QByteArray& salt, QString* error) {
    PKey privateKey = rawPrivate(privateBytes, EVP_PKEY_X25519);
    PKey peer = rawPublic(peerPublicKey, EVP_PKEY_X25519);
    PKeyContext deriveContext(EVP_PKEY_CTX_new(privateKey.get(), nullptr), EVP_PKEY_CTX_free);
    size_t sharedSize = 0;
    if (!privateKey || !peer || !deriveContext || EVP_PKEY_derive_init(deriveContext.get()) != 1 ||
        EVP_PKEY_derive_set_peer(deriveContext.get(), peer.get()) != 1 ||
        EVP_PKEY_derive(deriveContext.get(), nullptr, &sharedSize) != 1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to derive shared secret."));
        return {};
    }
    QByteArray shared(static_cast<qsizetype>(sharedSize), Qt::Uninitialized);
    if (EVP_PKEY_derive(deriveContext.get(), reinterpret_cast<unsigned char*>(shared.data()),
                        &sharedSize) != 1)
        return {};
    PKeyContext hkdf(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
    QByteArray result(32, Qt::Uninitialized);
    size_t resultSize = static_cast<size_t>(result.size());
    const QByteArray info("ketplus-session-key-v1");
    if (!hkdf || EVP_PKEY_derive_init(hkdf.get()) != 1 ||
        EVP_PKEY_CTX_hkdf_mode(hkdf.get(), EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(hkdf.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(hkdf.get(),
                                    reinterpret_cast<const unsigned char*>(salt.constData()),
                                    salt.size()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(hkdf.get(),
                                   reinterpret_cast<const unsigned char*>(shared.constData()),
                                   shared.size()) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(hkdf.get(),
                                    reinterpret_cast<const unsigned char*>(info.constData()),
                                    info.size()) != 1 ||
        EVP_PKEY_derive(hkdf.get(), reinterpret_cast<unsigned char*>(result.data()), &resultSize) !=
            1) {
        if (error != nullptr)
            *error = opensslError(QStringLiteral("Unable to derive wrapping key."));
        return {};
    }
    return result;
}

QJsonObject CloudCrypto::wrapSessionKey(const QString& sessionId, const qint64 generation,
                                        const int keyEpoch, const QJsonObject& recipient,
                                        const QString& senderDeviceId, const int senderKeyVersion,
                                        QString* error) {
    const QString recipientId = recipient.value(QStringLiteral("id")).toString();
    const int recipientVersion = recipient.value(QStringLiteral("keyVersion")).toInt();
    const QByteArray recipientPublic =
        fromBase64Url(recipient.value(QStringLiteral("keys"))
                          .toObject()
                          .value(QStringLiteral("agreementPublicKey"))
                          .toString());
    PKeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* rawEphemeral = nullptr;
    if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
        EVP_PKEY_keygen(context.get(), &rawEphemeral) != 1)
        return {};
    PKey ephemeral(rawEphemeral, EVP_PKEY_free);
    size_t privateSize = 32;
    size_t publicSize = 32;
    QByteArray ephemeralPrivate(32, Qt::Uninitialized);
    QByteArray ephemeralPublic(32, Qt::Uninitialized);
    if (EVP_PKEY_get_raw_private_key(ephemeral.get(),
                                     reinterpret_cast<unsigned char*>(ephemeralPrivate.data()),
                                     &privateSize) != 1 ||
        EVP_PKEY_get_raw_public_key(ephemeral.get(),
                                    reinterpret_cast<unsigned char*>(ephemeralPublic.data()),
                                    &publicSize) != 1)
        return {};
    const QByteArray wrappingKey =
        derive(ephemeralPrivate, recipientPublic, sessionId.toUtf8(), error);
    const QByteArray nonce = randomBytes(12, error);
    QByteArray tag;
    const QByteArray plainKey = sessionKey(sessionId, generation, error);
    const QByteArray ciphertext =
        plainKey.size() == 32 && wrappingKey.size() == 32 && nonce.size() == 12
            ? aesGcmEncrypt(plainKey, {}, wrappingKey, nonce, &tag, error)
            : QByteArray{};
    // Never sign or hand out a malformed wrap: it would be persisted and redistributed forever.
    if (ephemeralPublic.size() != 32 || nonce.size() != 12 || ciphertext.size() != 32 ||
        tag.size() != 16) {
        if (error != nullptr && error->isEmpty())
            *error = QStringLiteral("Unable to wrap the session key.");
        return {};
    }
    QJsonObject envelope{
        {QStringLiteral("version"), 1},
        {QStringLiteral("algorithm"), QStringLiteral("X25519-HKDF-SHA256-A256GCM")},
        {QStringLiteral("sessionId"), sessionId},
        {QStringLiteral("sessionGeneration"), generation},
        {QStringLiteral("keyEpoch"), keyEpoch},
        {QStringLiteral("recipientDeviceId"), recipientId},
        {QStringLiteral("recipientKeyVersion"), recipientVersion},
        {QStringLiteral("senderDeviceId"), senderDeviceId},
        {QStringLiteral("senderKeyVersion"), senderKeyVersion},
        {QStringLiteral("ephemeralPublicKey"), QString::fromLatin1(base64Url(ephemeralPublic))},
        {QStringLiteral("nonce"), QString::fromLatin1(base64Url(nonce))},
        {QStringLiteral("ciphertext"), QString::fromLatin1(base64Url(ciphertext))},
        {QStringLiteral("tag"), QString::fromLatin1(base64Url(tag))},
        {QStringLiteral("signature"), QString{}},
        {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    QByteArray signingInput =
        canonical({QStringLiteral("ketplus"), 2, QStringLiteral("wrapped-session-key"), sessionId,
                   generation, keyEpoch, recipientId, recipientVersion, senderDeviceId,
                   senderKeyVersion, QStringLiteral("X25519-HKDF-SHA256-A256GCM")});
    signingInput.append('\0');
    signingInput.append(ephemeralPublic);
    signingInput.append(nonce);
    signingInput.append(ciphertext);
    signingInput.append(tag);
    const QByteArray signature = sign(signingInput, error);
    if (signature.size() != 64)
        return {};
    envelope.insert(QStringLiteral("signature"), QString::fromLatin1(base64Url(signature)));
    return validWrappedSessionKey(envelope) ? envelope : QJsonObject{};
}

bool CloudCrypto::importSessionKey(const QJsonObject& envelope, const QByteArray& senderSigningKey,
                                   const QString& recipientDeviceId, const int recipientKeyVersion,
                                   QString* error) {
    if (!validWrappedSessionKey(envelope) ||
        envelope.value("recipientDeviceId").toString() != recipientDeviceId ||
        envelope.value("recipientKeyVersion").toInt() != recipientKeyVersion ||
        envelope.value("sessionGeneration").toInteger() < 1 ||
        envelope.value("keyEpoch").toInt() != 1 || envelope.value("senderKeyVersion").toInt() < 1) {
        if (error)
            *error = "Invalid wrapped session key scope.";
        return false;
    }
    const auto bytes = [&](const char* field) {
        return fromBase64Url(envelope.value(QLatin1String(field)).toString());
    };
    const auto session = envelope.value("sessionId").toString();
    const auto generation = envelope.value("sessionGeneration").toInteger();
    auto input =
        canonical({"ketplus", 2, "wrapped-session-key", session, generation, 1, recipientDeviceId,
                   recipientKeyVersion, envelope.value("senderDeviceId"),
                   envelope.value("senderKeyVersion"), envelope.value("algorithm")});
    input.append('\0');
    input.append(bytes("ephemeralPublicKey"));
    input.append(bytes("nonce"));
    input.append(bytes("ciphertext"));
    input.append(bytes("tag"));
    PKey sender = rawPublic(senderSigningKey, EVP_PKEY_ED25519);
    DigestContext verify(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    const auto signature = bytes("signature");
    if (!sender || !verify ||
        EVP_DigestVerifyInit(verify.get(), nullptr, nullptr, nullptr, sender.get()) != 1 ||
        EVP_DigestVerify(
            verify.get(), reinterpret_cast<const unsigned char*>(signature.constData()),
            signature.size(), reinterpret_cast<const unsigned char*>(input.constData()),
            input.size()) != 1) {
        if (error)
            *error = "Wrapped session key signature verification failed.";
        return false;
    }
    const auto local = privateKey("agreement-private-key", EVP_PKEY_X25519, error);
    const auto wrapping = derive(local, bytes("ephemeralPublicKey"), session.toUtf8(), error);
    const auto key =
        aesGcmDecrypt(bytes("ciphertext"), {}, wrapping, bytes("nonce"), bytes("tag"), error);
    if (key.size() != 32)
        return false;
    return store_->write(sessionKeyAccount(session, generation, 1), key, error);
}

bool CloudCrypto::validWrappedSessionKey(const QJsonObject& envelope) {
    const auto bytes = [&envelope](const char* field) {
        return fromBase64Url(envelope.value(QLatin1String(field)).toString());
    };
    return envelope.value(QStringLiteral("version")).toInt() == 1 &&
           envelope.value(QStringLiteral("algorithm")).toString() ==
               QStringLiteral("X25519-HKDF-SHA256-A256GCM") &&
           !envelope.value(QStringLiteral("sessionId")).toString().isEmpty() &&
           !envelope.value(QStringLiteral("recipientDeviceId")).toString().isEmpty() &&
           bytes("ephemeralPublicKey").size() == 32 && bytes("nonce").size() == 12 &&
           bytes("ciphertext").size() == 32 && bytes("tag").size() == 16 &&
           bytes("signature").size() == 64;
}

} // namespace ketplus::daemon
