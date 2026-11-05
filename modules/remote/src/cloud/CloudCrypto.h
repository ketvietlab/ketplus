#pragma once

#include "cloud/CredentialStore.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

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
    [[nodiscard]] QByteArray decryptSealed(const QJsonObject& content,
                                           const QByteArray& associatedData,
                                           const QByteArray& senderSigningKey,
                                           const QString& requestId, QString* error = nullptr);
    [[nodiscard]] QJsonObject
    encryptSealed(const QByteArray& plaintext, const QByteArray& associatedData,
                  const QByteArray& recipientAgreementKey, const QString& requestId,
                  const QString& recipientDeviceId, int recipientKeyVersion,
                  const QString& senderDeviceId, int senderKeyVersion, QString* error = nullptr);
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
