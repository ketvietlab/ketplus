#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// Control v2 primitives with keys passed in: no credential store, network or IPC. Built as
// ketplus_control_crypto_primitives (QtCore and OpenSSL only); CloudCrypto delegates to these.
// Keys are raw bytes. Error texts never contain key material.
namespace ketplus::control_crypto {

[[nodiscard]] QByteArray base64Url(const QByteArray& value);
[[nodiscard]] QByteArray fromBase64Url(const QString& value);
// The canonical encoding of signed associated data: compact JSON of the array.
[[nodiscard]] QByteArray canonical(const QJsonArray& values);
// base64url(SHA-256(signing public key || agreement public key)); empty unless both are 32 bytes.
[[nodiscard]] QString keyFingerprint(const QByteArray& signingPublicKey,
                                     const QByteArray& agreementPublicKey);
// The same for a key set with base64url `signingPublicKey` and `agreementPublicKey`.
[[nodiscard]] QString keyFingerprint(const QJsonObject& keys);

// Raw private and public keys of an OpenSSL raw-key type (EVP_PKEY_ED25519, EVP_PKEY_X25519).
[[nodiscard]] QByteArray generatePrivateKey(int keyType, QString* error = nullptr);
[[nodiscard]] QByteArray publicKey(const QByteArray& privateKey, int keyType,
                                   QString* error = nullptr);

struct Ed25519KeyPair {
    QByteArray privateKey; // 32-byte seed
    QByteArray publicKey;  // 32 bytes
};
// Both members are empty on failure.
[[nodiscard]] Ed25519KeyPair generateEd25519(QString* error = nullptr);
// A 64-byte signature, or empty on failure (including a private key of the wrong length).
[[nodiscard]] QByteArray signEd25519(const QByteArray& privateKey, const QByteArray& message,
                                     QString* error = nullptr);
// False for any malformed key or signature as well as a signature that does not verify.
[[nodiscard]] bool verifyEd25519(const QByteArray& publicKey, const QByteArray& message,
                                 const QByteArray& signature);

// `fallback`, followed by the first queued OpenSSL error if there is one.
[[nodiscard]] QString opensslError(const QString& fallback);

} // namespace ketplus::control_crypto
