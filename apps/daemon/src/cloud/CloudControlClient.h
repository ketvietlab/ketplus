#pragma once

#include "cloud/CloudCrypto.h"
#include "cloud/DevicePins.h"
#include "cloud/RelayWakeups.h"
#include "cloud/RemoteWorktreeQuota.h"
#include "cloud/SessionCredentials.h"
#include "transport/RemotePeers.h"
#include "transport/Signaling.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <QHash>

#include <atomic>
#include <functional>
#include <optional>

namespace ketplus::daemon {

class BrowserSignIn;
class LocalJournal;
class SessionHost;
struct StoredSession;

class CloudControlClient final : public QObject {
    Q_OBJECT

  public:
    using LocalCallback = std::function<void(const QJsonObject&, const QString&)>;

    CloudControlClient(LocalJournal* journal, SessionHost* host, QObject* parent = nullptr);
    ~CloudControlClient() override;

    void start();
    // The relay this build talks to: the KETPLUS_CONTROL_URL compiled in, or, in a Debug build
    // with the remote demo, the KETPLUS_CONTROL_URL environment variable.
    [[nodiscard]] static QUrl configuredControlUrl();
    [[nodiscard]] bool handles(const QString& messageType) const;
    void handleLocal(const QString& messageType, const QJsonObject& payload,
                     LocalCallback callback);

    // The relay drops an unclaimed Stop after 15s (interruptTtlSeconds); a Stop whose wake-up
    // is lost must still be polled before then.
    static constexpr int wakeupBackstopPollMilliseconds = 10'000;
    static constexpr int disconnectedPollMilliseconds = 2'000;
    static constexpr qsizetype machineChallengeBytes = 32;
    static constexpr int relayInterruptTtlMilliseconds = 15'000;
    static constexpr int senderKeyCacheMilliseconds = 10 * 60'000;
    static constexpr qsizetype maximumCachedSenderKeys = 64;
    static_assert(wakeupBackstopPollMilliseconds + 2 * disconnectedPollMilliseconds <
                      relayInterruptTtlMilliseconds,
                  "a Stop must be claimed before the relay expires it");

  private:
    friend class CloudPrivacyTestAccess;
    using ReplyCallback = std::function<void(int, const QJsonValue&, const QString&)>;
    using ReadyCallback = std::function<void(bool, const QString&)>;

    void request(const QByteArray& method, const QString& path, const QJsonValue& body,
                 const QString& bearer, const QHash<QByteArray, QByteArray>& headers,
                 ReplyCallback callback);
    void ensureDaemonCredential(ReadyCallback callback);
    void finishCredentialWaiters(bool ready, const QString& error);
    // Starts a browser sign-in and answers with the page to open; see CloudControlSignIn.cpp.
    void signIn(LocalCallback callback);
    void signInDevice(LocalCallback callback);
    void pollDeviceSignIn(quint64 signInEpoch);
    QTimer deviceSignInTimer_;
    QString deviceCode_;
    QString deviceVerifier_;
    QString deviceInstallationId_;
    QJsonObject deviceKeys_;
    QDateTime deviceSignInExpires_;
    int devicePollInterval_{5000};
    bool devicePollInFlight_{false};
    bool deviceSignInStarting_{false};
    void cancelSignIn(LocalCallback callback);
    void dropBrowserSignIn();
    void finishSignIn(quint64 signInEpoch, const QString& error);
    void redeemSignIn(quint64 signInEpoch, const QString& code, const QString& verifier,
                      const QString& redirectUri, const QString& installationId,
                      const QJsonObject& keys);
    void enrollMachine(quint64 signInEpoch, const QString& access, const QJsonObject& subject,
                       const QString& installationId, const QJsonObject& keys,
                       LocalCallback callback);
    void signOut(LocalCallback callback);
    void listDevices(LocalCallback callback);
    void createPairing(LocalCallback callback);
    void pairingStatus(const QJsonObject& payload, LocalCallback callback);
    void approvePairing(const QJsonObject& payload, LocalCallback callback);
    void revokePairing(const QJsonObject& payload, LocalCallback callback);
    void revokeDevice(const QJsonObject& payload, LocalCallback callback);
    QString uploadScope() const;
    qint64 uploadedCursor(const QString& sessionId, qint64 generation);
    bool registerCloudCursors();
    bool registerCloudCursor(const StoredSession& session);
    void initializePeerTransport();
    void negotiatePeerTransport(const QJsonValue& heartbeat);
    transport::EventPage peerEvents(const QString& sessionId, qint64 generation, qint64 cursor);
    QJsonObject sealEvent(const QJsonObject& event, const QString& sessionId, qint64 generation,
                          QString* error);
    void sync();
    void heartbeat();
    void sendHeartbeatBatches(const QList<QJsonArray>& batches, qsizetype index,
                              quint64 accountEpoch, const QString& machineId,
                              const QString& workspaceId);
    void uploadEvents();
    void uploadSessionEvents(const QString& sessionId, qint64 generation, qint64 afterCursor);
    // A deleted generation's history awaits purge but must never leave the machine (ADR 0093).
    [[nodiscard]] bool isLiveGeneration(const QString& sessionId, qint64 generation) const;
    void distributeKeys();
    void pollCommands();
    // A wake-up that lands while a poll is in flight polls again once it returns: that poll
    // may have read before the command it announces was written.
    void wakeCommandPoll();
    void noteCommandPollFailure(int status);
    // The active rows of a relay device list that are pinned here, adopting them on first use.
    QJsonArray trustedDevices(const QJsonArray& devices);
    using SenderKeysCallback = std::function<void(const QJsonObject&, const QString&)>;
    // A phone's published keys, read from the relay at most once per senderKeyCacheMilliseconds.
    void withSenderKeys(const QString& senderId, int version, SenderKeysCallback callback);
    void executeCommand(const QJsonObject& command);
    void executeWorkspaceFiles(const QJsonObject& command, const QJsonObject& payload);
    void notePublishedRevisions(const QList<QJsonArray>& batches);
    [[nodiscard]] bool revisionPublished(const QString& sessionId, const QString& revision) const;
    void executeRestoreWindow(const QJsonObject& command, const QJsonObject& payload);
    // nullopt: the reassembled large item is cloud-safe as raw bytes; otherwise its projection.
    [[nodiscard]] std::optional<QJsonObject> largeEventCloudProjection(const QString& sessionId,
                                                                       qint64 generation,
                                                                       const QString& transferId,
                                                                       const QJsonObject& manifest);
    void uploadRestoreResponses(const QJsonObject& command, const QJsonArray& responses,
                                const QJsonObject& localResult);
    void acknowledgeCommand(const QJsonObject& command, const QJsonObject& localResult,
                            const QByteArray& recipientAgreementKey = {});
    [[nodiscard]] QJsonObject status() const;
    bool clearUploadProgress(QString* error);
    // Removes what the credential file keeps for conversations no longer in the journal (#84).
    void pruneSessionCredentials();
    // Stops the Atlas runtimes, and with them their tunnels, that `matches` selects.
    void stopAtlasRuntimes(const std::function<bool(const QObject& runtime)>& matches);
    // Ends Atlas access for one device, or for every device when `device` is empty: running
    // runtimes stop, and an atlas.start still scanning for them creates none (#105).
    void revokeAtlasAccess(const QString& device = {});
    // What an atlas.start captures before its scan and checks before creating a runtime.
    struct AtlasAccessFence final {
        quint64 machine{0};
        quint64 device{0};
    };
    [[nodiscard]] AtlasAccessFence atlasAccessFence(const QString& device) const;
    [[nodiscard]] bool atlasAccessCurrent(const QString& device,
                                          const AtlasAccessFence& fence) const;
    [[nodiscard]] static QJsonArray
    sessionSnapshots(const QList<StoredSession>& stored, const QString& workspaceId,
                     const QString& machineId, const QByteArray& pathIdSalt,
                     const QHash<QString, QString>& repositories,
                     QHash<QString, QJsonObject>* metadata = nullptr,
                     const QHash<QString, QJsonObject>& agentMetadata = {},
                     const QHash<QString, QString>& tagColors = {});
    // Seals each snapshot's metadata (branch, machine name, whether its worktree still
    // exists, conversation number, the conversation whose message started its turn) for the
    // phone; a snapshot whose metadata cannot be sealed is
    // sent without it.
    [[nodiscard]] QList<QJsonArray>
    withSessionMetadata(const QList<QJsonArray>& batches,
                        const QHash<QString, QJsonObject>& metadata);
    [[nodiscard]] QJsonObject sealedSessionMetadata(const QJsonObject& snapshot,
                                                    const QJsonObject& metadata);
    QByteArray ensurePathIdSalt(QString* error = nullptr);
    // Where a conversation can start: every checkout of each repository a conversation
    // ran in, those with conversations first.
    [[nodiscard]] QStringList catalogWorktreePaths() const;
    [[nodiscard]] QJsonArray worktreeSnapshots() const;
    // session.create and worktree.branches.read: checked against the catalog worktree they
    // name, answered sealed to the phone that asked.
    void executeWorktreeCommand(const QJsonObject& command, const QJsonObject& payload,
                                const QByteArray& senderAgreementKey);
    void startCreatedConversation(const QJsonObject& command, const QJsonObject& payload,
                                  const QString& worktreePath,
                                  const QByteArray& senderAgreementKey);
    [[nodiscard]] QByteArray eventAssociatedData(const QJsonObject& event,
                                                 const QJsonObject& content) const;
    [[nodiscard]] QByteArray commandAssociatedData(const QJsonObject& command) const;
    [[nodiscard]] QByteArray commandResultAssociatedData(const QJsonObject& command, int attempt,
                                                         const QString& status,
                                                         const QString& resultCode,
                                                         const QJsonObject& content) const;
    [[nodiscard]] QByteArray restoreResponseAssociatedData(const QJsonObject& command,
                                                           qint64 responseCursor, bool terminal,
                                                           const QString& responseDigest,
                                                           const QJsonObject& content) const;
    [[nodiscard]] QString storedString(const QString& account) const;
    bool storeString(const QString& account, const QString& value, QString* error = nullptr);

    LocalJournal* journal_{nullptr};
    SessionHost* host_{nullptr};
    CredentialStore credentials_;
    CloudCrypto crypto_;
    DevicePins pins_;
    SessionCredentials sessionCredentials_;
    QNetworkAccessManager network_;
    QNetworkAccessManager* transport_{&network_};
    QTimer syncTimer_;
    QTimer commandTimer_;
    QTimer stateSyncTimer_;
    QUrl baseUrl_;
    RelayWakeups wakeups_;
    transport::RemotePeers* peers_{nullptr};
    transport::Signaling* signaling_{nullptr};
    QString machineId_;
    QString workspaceId_;
    // Who signed this Mac in, from the user session that enrolled it (ADR 0109).
    QString accountName_;
    QString accountUsername_;
    QString daemonToken_;
    QString lastError_;
    QPointer<BrowserSignIn> browserSignIn_;
    QString signInError_;
    // The relay a stored enrollment belongs to, when it is not this build's relay.
    QString otherRelay_;
    QByteArray pathIdSalt_;
    QDateTime daemonTokenExpiresAt_;
    QList<ReadyCallback> credentialWaiters_;
    bool credentialRequestInFlight_{false};
    bool heartbeatInFlight_{false};
    bool commandPollInFlight_{false};
    bool commandPollAgain_{false};
    // Consecutive failed polls or credential requests, and when the timer may poll again.
    int commandPollFailures_{0};
    QDeadlineTimer commandPollRetryAt_{0};
    QSet<QString> eventUploadsInFlight_;
    // session:generation pairs whose cloud cursor this process has registered with the journal.
    QSet<QString> registeredCloudCursors_;
    // session:generation -> events per upload after the relay refused a batch as too large.
    QHash<QString, int> eventUploadLimits_;
    struct EventUploadBackoff {
        int failures{0};
        bool refused{false};
        QDeadlineTimer retryAt;
    };
    // session:generation -> consecutive failed uploads and when the next one may be sent.
    QHash<QString, EventUploadBackoff> eventUploadBackoff_;
    // Sessions the last heartbeat was refused for; their events and keys stay local.
    QSet<QString> refusedSessions_;
    QSet<QString> refusalsThisHeartbeat_;
    // session:generation:transfer:digest -> large-event cloud decision (immutable once written).
    QHash<QString, std::optional<QJsonObject>> largeEventCloudDecisions_;
    // session -> (what was sealed, sealed metadata); resealed only when either changes.
    QHash<QString, std::pair<QByteArray, QJsonObject>> sealedMetadata_;
    bool sessionMetadataRefused_{false};
    // "device/version" -> (published keys, when to read them again).
    QHash<QString, std::pair<QJsonObject, QDateTime>> senderKeys_;
    // Command id -> when this daemon claimed it, for the result log line.
    QHash<QString, qint64> commandStartedAt_;
    RemoteWorktreeQuota worktreeQuota_;
    int machineKeyVersion_{1};
    quint64 accountEpoch_{0};
    // Raised by revokeAtlasAccess, for the machine and for each revoked device.
    quint64 atlasMachineEpoch_{0};
    QHash<QString, quint64> atlasDeviceEpochs_;
    QThreadPool workspaceFilesPool_;
    // Heartbeats hash every worktree; on their own thread they never queue a phone's read.
    QThreadPool heartbeatPool_;
    std::atomic_int workspaceFilesPending_{0};
    // session -> (workspace revision, ms since epoch) this daemon recently published for it.
    QHash<QString, QList<std::pair<QString, qint64>>> publishedRevisions_;
};

} // namespace ketplus::daemon
