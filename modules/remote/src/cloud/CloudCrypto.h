#pragma once

#include "cloud/CredentialStore.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace ketplus::daemon {

class CloudCrypto final {
  public:
    explicit CloudCrypto(CredentialStore* store);

    [[nodiscard]] QJsonObject publicKeys(QString* error = nullptr);
    [[nodiscard]] QByteArray sign(const QByteArray& bytes, QString* error = nullptr);
    [[nodiscard]] QByteArray sessionKey(const QString& sessionId, qint64 generation,
                                        QString* error = nullptr);
    [[nodiscard]] QJsonObject encrypt(const QByteArray& plaintext, const QByteArray& associatedData,
                                      const QString& sessionId, qint64 generation, int keyEpoch,
                                      const QString& senderDeviceId, int senderKeyVersion,
                                      QString* error = nullptr);
    [[nodiscard]] QByteArray decrypt(const QJsonObject& content, const QByteArray& associatedData,
                                     const QByteArray& senderSigningKey, const QString& sessionId,
                                     qint64 generation, QString* error = nullptr);
    // Sealed to an agreement key rather than a session key: content named
    // `session-create:<requestId>`, the key derived with `requestId` as its salt.
    [[nodiscard]] QByteArray decryptSealed(const QJsonObject& content,
                                           const QByteArray& associatedData,
                                           const QByteArray& senderSigningKey,
                                           const QString& requestId, QString* error = nullptr);
    [[nodiscard]] QJsonObject
    encryptSealed(const QByteArray& plaintext, const QByteArray& associatedData,
                  const QByteArray& recipientAgreementKey, const QString& requestId,
                  const QString& recipientDeviceId, int recipientKeyVersion,
                  const QString& senderDeviceId, int senderKeyVersion, QString* error = nullptr);
    // The same with the content's key id named by the caller, such as
    // `team-command:<requestId>` and `team-result:<requestId>` (ADR 0116 §2).
    [[nodiscard]] QByteArray decryptSealedAs(const QJsonObject& content,
                                             const QByteArray& associatedData,
                                             const QByteArray& senderSigningKey,
                                             const QString& requestId, const QString& keyId,
                                             QString* error = nullptr);
    [[nodiscard]] QJsonObject
    encryptSealedAs(const QByteArray& plaintext, const QByteArray& associatedData,
                    const QByteArray& recipientAgreementKey, const QString& requestId,
                    const QString& keyId, const QString& recipientDeviceId, int recipientKeyVersion,
                    const QString& senderDeviceId, int senderKeyVersion, QString* error = nullptr);
    // The same, for content whose associated data names its own envelope (a command sealed to a
    // machine binds recipientDeviceId, recipientKeyVersion and ephemeralPublicKey): the callback
    // receives every envelope field except nonce, ciphertext, tag and signature.
    using SealedAssociatedData = std::function<QByteArray(const QJsonObject& envelope)>;
    [[nodiscard]] QJsonObject
    encryptSealedBound(const QByteArray& plaintext, const SealedAssociatedData& associatedData,
                       const QByteArray& recipientAgreementKey, const QString& requestId,
                       const QString& keyId, const QString& recipientDeviceId,
                       int recipientKeyVersion, const QString& senderDeviceId, int senderKeyVersion,
                       QString* error = nullptr);
    [[nodiscard]] QJsonObject wrapSessionKey(const QString& sessionId, qint64 generation,
                                             int keyEpoch, const QJsonObject& recipient,
                                             const QString& senderDeviceId, int senderKeyVersion,
                                             QString* error = nullptr);

    // Structural check (field lengths/algorithm) for a wrapped session-key envelope; applied
    // before signing/persisting and when reading a cached envelope back.
    [[nodiscard]] static bool validWrappedSessionKey(const QJsonObject& envelope);
    // Remote clients import only a host-signed key addressed to their current identity.
    bool importSessionKey(const QJsonObject& envelope, const QByteArray& senderSigningKey,
                          const QString& recipientDeviceId, int recipientKeyVersion,
                          QString* error = nullptr);
    [[nodiscard]] static QByteArray base64Url(const QByteArray& value);
    [[nodiscard]] static QByteArray fromBase64Url(const QString& value);
    [[nodiscard]] static QByteArray canonical(const QJsonArray& values);
    // base64url(SHA-256(signing public key || agreement public key)) of a key set: the host
    // identity a pairing QR code carries and clients pin (#89, #88). Empty for invalid keys.
    [[nodiscard]] static QString keyFingerprint(const QJsonObject& keys);

  private:
    [[nodiscard]] QByteArray privateKey(const QString& account, int keyType, QString* error);
    [[nodiscard]] QByteArray publicKey(const QByteArray& privateKey, int keyType, QString* error);
    [[nodiscard]] QByteArray derive(const QByteArray& privateKey, const QByteArray& peerPublicKey,
                                    const QByteArray& salt, QString* error);
    [[nodiscard]] static QString sessionKeyAccount(const QString& sessionId, qint64 generation,
                                                   int keyEpoch);

    CredentialStore* store_{nullptr};
};

} // namespace ketplus::daemon
