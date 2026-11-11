#pragma once
#include "cloud/CloudControlClient.h"
#include "storage/LocalJournal.h"
#include <QDateTime>
#include <QJsonArray>
#include <functional>
namespace ketplus::daemon {
class CloudPrivacyTestAccess {
  public:
    static transport::RemotePeers& peers(CloudControlClient& client) { return *client.peers_; }
    static transport::EventPage peerEvents(CloudControlClient& client, const QString& id,
                                           qint64 gen, qint64 cursor) {
        return client.peerEvents(id, gen, cursor);
    }
    static void deviceTransport(CloudControlClient& client, QNetworkAccessManager& transport) {
        client.transport_ = &transport;
        client.baseUrl_ = QUrl("https://synthetic.invalid/native/ketplus");
    }
    static void pollDevice(CloudControlClient& client) {
        client.deviceSignInTimer_.stop();
        client.pollDeviceSignIn(client.accountEpoch_);
    }
    static int deviceInterval(const CloudControlClient& client) {
        return client.devicePollInterval_;
    }
    static bool devicePollInFlight(const CloudControlClient& client) {
        return client.devicePollInFlight_;
    }
    static void expireDevice(CloudControlClient& client) {
        client.deviceSignInExpires_ = QDateTime::currentDateTimeUtc().addSecs(-1);
    }
    static void configure(CloudControlClient& client, QNetworkAccessManager& transport,
                          const QUrl& origin = QUrl("https://synthetic.invalid")) {
        client.transport_ = &transport;
        client.baseUrl_ = origin;
        client.machineId_ = "machine-synthetic";
        client.workspaceId_ = "workspace-synthetic";
        client.daemonToken_ = "synthetic-test-token";
        client.daemonTokenExpiresAt_ = QDateTime::currentDateTimeUtc().addSecs(3600);
    }
    static void upload(CloudControlClient& client, const QString& session = "session-synthetic",
                       qint64 generation = 1) {
        client.uploadSessionEvents(session, generation, 0);
    }
    // Starts an upload for every session behind, as each journaled event does.
    static void uploadAll(CloudControlClient& client) { client.uploadEvents(); }
    // Ends every wait after a failed upload, as if its time had passed.
    static void expireUploadBackoff(CloudControlClient& client) {
        for (auto& backoff : client.eventUploadBackoff_)
            backoff.retryAt = QDeadlineTimer(0);
    }
    // Sends one heartbeat batch per entry, as the heartbeat timer does after its snapshot.
    static void heartbeat(CloudControlClient& client, const QList<QJsonArray>& batches) {
        client.heartbeatInFlight_ = true;
        client.refusalsThisHeartbeat_.clear();
        client.sendHeartbeatBatches(batches, 0, client.accountEpoch_, client.machineId_,
                                    client.workspaceId_);
    }
    static bool heartbeatIdle(const CloudControlClient& client) {
        return !client.heartbeatInFlight_;
    }
    static bool idle(const CloudControlClient& client) {
        return client.eventUploadsInFlight_.isEmpty();
    }
    static CloudCrypto& crypto(CloudControlClient& client) { return client.crypto_; }
    static RelayWakeups& wakeups(CloudControlClient& client) { return client.wakeups_; }
    static CloudControlClient::AtlasAccessFence atlasFence(const CloudControlClient& client,
                                                           const QString& device) {
        return client.atlasAccessFence(device);
    }
    static bool atlasAccessCurrent(const CloudControlClient& client, const QString& device,
                                   const CloudControlClient::AtlasAccessFence& fence) {
        return client.atlasAccessCurrent(device, fence);
    }
    static QByteArray aad(CloudControlClient& client, const QJsonObject& event,
                          const QJsonObject& content) {
        return client.eventAssociatedData(event, content);
    }
    static void restore(CloudControlClient& client, const QJsonObject& command,
                        const QJsonObject& payload) {
        client.executeRestoreWindow(command, payload);
    }
    static QByteArray restoreAad(CloudControlClient& client, const QJsonObject& command,
                                 qint64 cursor, bool terminal, const QString& digest,
                                 const QJsonObject& content) {
        return client.restoreResponseAssociatedData(command, cursor, terminal, digest, content);
    }
    static QByteArray commandAad(CloudControlClient& client, const QJsonObject& command) {
        return client.commandAssociatedData(command);
    }
    static QString error(const CloudControlClient& client) { return client.lastError_; }
    static void validCredential(CloudControlClient& client) {
        client.daemonTokenExpiresAt_ = QDateTime::currentDateTimeUtc().addSecs(3600);
    }
    static void acknowledge(CloudControlClient& client, const QJsonObject& command,
                            const QJsonObject& result) {
        client.acknowledgeCommand(command, result);
    }
    static void execute(CloudControlClient& client, const QJsonObject& command) {
        client.executeCommand(command);
    }
    static void workspaceFiles(CloudControlClient& client, const QJsonObject& command,
                               const QJsonObject& payload) {
        client.executeWorkspaceFiles(command, payload);
    }
    static void worktreeCommand(CloudControlClient& client, const QJsonObject& command,
                                const QJsonObject& payload, const QByteArray& senderKey) {
        client.executeWorktreeCommand(command, payload, senderKey);
    }
    static void teamCommand(CloudControlClient& client, const QJsonObject& command,
                            const QJsonObject& payload, const QByteArray& senderKey) {
        client.executeTeamCommand(command, payload, senderKey);
    }
    static QByteArray resultAad(CloudControlClient& client, const QJsonObject& command, int attempt,
                                const QString& status, const QString& code,
                                const QJsonObject& content) {
        return client.commandResultAssociatedData(command, attempt, status, code, content);
    }
    static bool teamSyncPending(const CloudControlClient& client) {
        return client.teamSyncTimer_.isActive();
    }
    static bool stateSyncPending(const CloudControlClient& client) {
        return client.stateSyncTimer_.isActive();
    }
    static RemoteWorktreeQuota& worktreeQuota(CloudControlClient& client) {
        return client.worktreeQuota_;
    }
    static void workspace(CloudControlClient& client, const QString& workspace) {
        client.workspaceId_ = workspace;
    }
    static void pathSalt(CloudControlClient& client, const QByteArray& salt) {
        client.pathIdSalt_ = salt;
    }
    static void machineKeyVersion(CloudControlClient& client, int version) {
        client.machineKeyVersion_ = version;
    }
    static QJsonObject status(const CloudControlClient& client) { return client.status(); }
    static void senderKeys(CloudControlClient& client, const QString& sender, int version,
                           std::function<void(const QJsonObject&, const QString&)> callback) {
        client.withSenderKeys(sender, version, std::move(callback));
    }
    static void expireSenderKeys(CloudControlClient& client) {
        for (auto& entry : client.senderKeys_)
            entry.second = QDateTime::currentDateTimeUtc().addSecs(-1);
    }
    // Pins a device on this Mac, as approving its pairing does.
    static void pinDevice(CloudControlClient& client, const QString& id, int version,
                          const QJsonObject& keys) {
        if (!client.pins_.pin({{"id", id}, {"keyVersion", version}, {"keys", keys}}))
            qFatal("Synthetic device pin failed");
    }
    static DevicePins& pins(CloudControlClient& client) { return client.pins_; }
    static QString machineId(const CloudControlClient& client) { return client.machineId_; }
    static void distributeKeys(CloudControlClient& client) { client.distributeKeys(); }
    static void createPairing(CloudControlClient& client, CloudControlClient::LocalCallback done) {
        client.createPairing(std::move(done));
    }
    static void pairingStatus(CloudControlClient& client, const QString& ticketId,
                              CloudControlClient::LocalCallback done) {
        client.pairingStatus({{"ticket_id", ticketId}}, std::move(done));
    }
    static void approvePairing(CloudControlClient& client, const QString& ticketId,
                               const QString& confirmedDigest,
                               CloudControlClient::LocalCallback done) {
        client.approvePairing({{"ticket_id", ticketId}, {"claim_digest", confirmedDigest}},
                              std::move(done));
    }
    // Enrolls with the relay as a completed sign-in does.
    static void enroll(CloudControlClient& client, CloudControlClient::LocalCallback done,
                       const QJsonObject& subject = {}) {
        QString error;
        const QJsonObject keys = client.crypto_.publicKeys(&error);
        client.enrollMachine(client.accountEpoch_, "synthetic-user-token", subject,
                             "host_synthetic", keys, std::move(done));
    }
    // Asks for a fresh daemon credential, as an expired token does.
    static void credential(CloudControlClient& client, CloudControlClient::ReadyCallback done) {
        client.daemonToken_.clear();
        client.ensureDaemonCredential(std::move(done));
    }
    static void pollCommands(CloudControlClient& client) { client.pollCommands(); }
    static void wakeCommandPoll(CloudControlClient& client) { client.wakeCommandPoll(); }
    static bool commandPollInFlight(const CloudControlClient& client) {
        return client.commandPollInFlight_;
    }
    static bool commandPollWaiting(const CloudControlClient& client) {
        return !client.commandPollRetryAt_.hasExpired();
    }
    static int commandPollFailures(const CloudControlClient& client) {
        return client.commandPollFailures_;
    }
    static void expireCommandPollBackoff(CloudControlClient& client) {
        client.commandPollRetryAt_ = QDeadlineTimer(0);
    }
    static qsizetype cachedSenderKeys(const CloudControlClient& client) {
        return client.senderKeys_.size();
    }
    static QJsonObject metadata(const StoredSession& session,
                                const QHash<QString, QJsonObject>& agentMetadata = {},
                                const QHash<QString, QString>& tagColors = {}) {
        QHash<QString, QJsonObject> sealed;
        CloudControlClient::sessionSnapshots({session}, "workspace-synthetic", "machine-synthetic",
                                             "salt", {}, &sealed, agentMetadata, tagColors);
        return sealed.value(session.id);
    }
    static QJsonArray snapshots(const QList<StoredSession>& sessions) {
        return CloudControlClient::sessionSnapshots(sessions, "workspace-synthetic",
                                                    "machine-synthetic", "salt", {}, nullptr);
    }
};
} // namespace ketplus::daemon
