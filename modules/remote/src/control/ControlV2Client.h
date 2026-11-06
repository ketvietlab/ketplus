#pragma once
#include "cloud/CloudCrypto.h"
#include <QHash>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>

namespace ketplus::daemon {
class BrowserSignIn;
}

namespace ketplus::remote {
// Qt backend for a paired Desktop acting as a remote client. It never resolves host paths,
// starts a local daemon, or hands credentials/transcript contents to the Desktop UI implicitly.
class ControlV2Client final : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const QJsonValue&, const QString&)>;
    explicit ControlV2Client(QUrl origin, QObject* parent = nullptr);
    [[nodiscard]] QJsonObject identity(QString* error = nullptr);
    // The browser-auth adapter uses identity().installationId to obtain this UserSession.
    // Account replacement fences outstanding requests and drops the previous pairing.
    bool setCredential(const QJsonObject& credential, QString* error = nullptr);
    [[nodiscard]] bool signedIn() const { return !credential_.isEmpty(); }
    // Returns the relay-owned authorization URL; the UI opens it in the system browser.
    void startSignIn(Callback done);
    void cancelSignIn();
    void signOut(Callback done);
    // host is what the pairing QR code names: {machineId, keyVersion, fingerprint}. The host's
    // keys are pinned to it when the pairing is accepted (#88).
    void claimPairing(const QString& ticketId, const QString& secret, const QString& displayName,
                      const QJsonObject& host, Callback done);
    void pairingStatus(const QString& ticketId, Callback done);
    void cancelPairing();
    bool acceptPairing(const QJsonObject& approvedTicket, QString* error = nullptr);
    void listSessions(Callback done);
    void listWorktrees(Callback done);
    void events(const QJsonObject& session, qint64 afterCursor, Callback done);
    // Persist the prepared encrypted envelope before sending and retry that exact ciphertext.
    void prepareCommand(const QJsonObject& intent, const QJsonObject& plaintext, Callback done);
    void submit(const QJsonObject& encryptedIntent, Callback done);
    void command(const QJsonObject& submittedIntent, Callback done);
    [[nodiscard]] static QByteArray commandAssociatedData(const QJsonObject& intent);

  signals:
    void signInFinished(const QString& error);

  private:
    friend class RemoteControlTestAccess;
    using Reply = std::function<void(int, const QJsonValue&, const QString&)>;
    void request(const QByteArray& method, const QString& path, const QJsonValue& body,
                 const QString& bearer, Reply done);
    void authorized(const QByteArray& method, const QString& path, const QJsonValue& body,
                    Callback done);
    void accessToken(std::function<void(const QString&, const QString&)> done);
    void sessionKeys(const QJsonObject& session,
                     std::function<void(const QByteArray&, const QString&)> done);
    void senderKey(const QString& id, int version,
                   std::function<void(const QByteArray&, const QString&)> done);
    bool validatePairing(const QJsonObject& ticket, QString* error);
    QUrl origin_;
    daemon::CredentialStore credentials_;
    daemon::CloudCrypto crypto_{&credentials_};
    QNetworkAccessManager network_;
    QNetworkAccessManager* transport_{&network_};
    QJsonObject credential_;
    QHash<QString, QJsonObject> pairingClaims_;
    // Ticket id -> the host its QR code named.
    QHash<QString, QJsonObject> pairingHosts_;
    // A rotated credential that could not be stored yet; written again on the next access.
    bool credentialUnsaved_{false};
    QString installationId_;
    QString deviceId_;
    int keyVersion_{1};
    quint64 epoch_{0};
    quint64 pairingEpoch_{0};
    QPointer<daemon::BrowserSignIn> browserSignIn_;
    quint64 signInEpoch_{0};
    bool refreshing_{false};
    QList<std::function<void(const QString&, const QString&)>> refreshWaiters_;
};
} // namespace ketplus::remote
