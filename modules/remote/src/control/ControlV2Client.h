#pragma once
#include "cloud/CloudCrypto.h"
#include <QHash>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>
#include <memory>

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
    // The paired hosts' repositories/worktrees and conversations, as
    // {repositories, conversations, rejected}. Conversation title, state, updated time and
    // branch come from the host-signed session metadata; a row whose metadata, host pin or
    // index entry fails authentication is left out and named in `rejected`, never shown.
    void projection(Callback done);
    // One bounded transcript window over Control v2, shaped as the local daemon's
    // session.transcript_window page. `window` takes snapshot_cursor (defaults to the session's
    // lastEventCursor), before_cursor (0 = newest turn), event_limit (<= 128), turn_limit
    // (<= 3) and byte_cap (4 KiB..512 KiB). Page backwards with the previous page's
    // before_cursor and the same snapshot_cursor. Large events are reassembled first.
    void transcriptWindow(const QJsonObject& session, const QJsonObject& window, Callback done);
    // Persist the prepared encrypted envelope before sending and retry that exact ciphertext.
    void prepareCommand(const QJsonObject& intent, const QJsonObject& plaintext, Callback done);
    void submit(const QJsonObject& encryptedIntent, Callback done);
    void command(const QJsonObject& submittedIntent, Callback done);
    [[nodiscard]] static QByteArray commandAssociatedData(const QJsonObject& intent);

  signals:
    void signInFinished(const QString& error);

  private:
    friend class RemoteControlTestAccess;
    struct ProjectionState;
    struct RestoreState;
    void projectNext(const std::shared_ptr<ProjectionState>& state);
    void finishProjection(const std::shared_ptr<ProjectionState>& state);
    [[nodiscard]] bool hostPinned(const QString& machineId) const;
    void startRestore(const std::shared_ptr<RestoreState>& state, const QJsonObject& request,
                      const QJsonObject& payload);
    void pollRestore(const std::shared_ptr<RestoreState>& state);
    void restoreCompleted(const std::shared_ptr<RestoreState>& state, const QByteArray& bytes);
    void nextLargeEvent(const std::shared_ptr<RestoreState>& state);
    void requestLargeFragment(const std::shared_ptr<RestoreState>& state);
    void failRestore(const std::shared_ptr<RestoreState>& state, const QString& error);
    // "session:generation" -> host signing key version that sessionKeys() verified against a
    // pin. Content naming another version fails closed: it needs a new pairing (#88).
    QHash<QString, int> signerVersions_;
    [[nodiscard]] bool signedByVerifiedKey(const QString& sessionId, qint64 generation,
                                           const QJsonObject& content) const;
    // Restore responses are polled while the host works; tests shorten the interval.
    int restorePollMilliseconds_{250};
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
    // "machine/version" -> host key fingerprint, written only by this client when it accepts a
    // pairing, in its own credential account: nothing the relay returns can add one (#88).
    QJsonObject hostPins_;
    bool hostPinsStored_{false};
    bool storeHostPins(const QJsonObject& pins, QString* error);
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
