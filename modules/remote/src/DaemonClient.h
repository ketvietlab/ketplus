#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace ketplus::remote {

class DaemonClient final : public QObject {
    Q_OBJECT

  public:
    using ResponseCallback = std::function<void(const QJsonObject&, const QString&)>;

    // Supplies the daemon's local credential for daemon.hello; held only in memory.
    using CredentialSource = std::function<QByteArray()>;

    // This Mac's daemon: connects to its socket, starts it when absent and reconnects itself.
    explicit DaemonClient(QObject* parent = nullptr);
    // A remote daemon whose route (SSH) owns the connection: the client never starts, replaces or
    // reconnects a daemon. Calls queue until adoptConnection() hands it a connected socket and the
    // hello succeeds, and they replay after a disconnect as they do locally.
    explicit DaemonClient(CredentialSource credential, QObject* parent = nullptr);
    ~DaemonClient() override;

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] bool isReady() const noexcept;
    // Negotiated at daemon.hello (ADR 0015 step 5); 0 and empty until then or with an older daemon.
    [[nodiscard]] int protocolMinor() const noexcept { return protocolMinor_; }
    [[nodiscard]] bool hasFeature(const QString& feature) const {
        return features_.contains(feature);
    }
    // ADR 0119 "Machine identity": the host key fingerprint the daemon named at daemon.hello
    // (daemon.host_identity/1); empty until then or from a daemon that names none.
    [[nodiscard]] QString hostKeyFingerprint() const { return hostKeyFingerprint_; }
    [[nodiscard]] bool hasCapability(const QString& capability) const {
        return ready_ && capabilities_.contains(capability);
    }
    // ADR 0119 "Host key rotation": the machine id a remote route last trusted. Set, a hello that
    // names another fingerprint (or none) fails closed: nothing queued is sent, every call fails
    // with identityRejectedReason() and the client stops reconnecting until this is set again.
    void setTrustedHostKeyFingerprint(const QString& fingerprint);
    [[nodiscard]] bool identityRejected() const noexcept { return identityRejected_; }
    // A remote route names its machine by this fingerprint, so a hello that names none fails
    // closed like a changed one. The first fingerprint seen is trusted for the rest of the session.
    void setHostIdentityRequired(bool required) { hostIdentityRequired_ = required; }
    // Fails the calls still waiting for a hello with `reason`, for a route that went Offline.
    void failWaitingCalls(const QString& reason);
    [[nodiscard]] static QString identityRejectedReason();
    // Set before connecting; only transcript-aware Desktop opts into summary-only events.
    void setActivityGroupsEnabled(bool enabled) { activityGroupsEnabled_ = enabled; }
    void call(const QString& type, const QJsonObject& payload, const QString& sessionId,
              qint64 generation, ResponseCallback callback);
    // Remote clients only: speak the protocol over `socket`, which stays owned by the route. A
    // new socket replaces the previous one; null ends the current connection.
    void adoptConnection(QLocalSocket* socket);

    [[nodiscard]] static QString defaultSocketPath();
    [[nodiscard]] static QString defaultStatePath();
    [[nodiscard]] static QString defaultTokenPath();

  signals:
    void readyChanged(bool ready);
    void eventReceived(const QJsonObject& event);
    // ADR 0114: team.changed and team.removed, sent to every client without a session.
    void teamEventReceived(const QString& type, const QJsonObject& payload);
    // ADR 0126: schedule.changed and schedule.run, sent without a session.
    void scheduleEventReceived(const QString& type, const QJsonObject& payload);
    // A daemon push that is not a session event, such as `tags.changed` (#429).
    void noticeReceived(const QString& type, const QJsonObject& payload);
    void disconnected(const QString& reason);
    // The hello named `presented` instead of the trusted fingerprint (empty: it named none).
    void hostIdentityRejected(const QString& presented);

  private:
    friend class DaemonClientTestAccess;

    struct QueuedCall final {
        QString messageId;
        QString type;
        QJsonObject payload;
        QString sessionId;
        qint64 generation{-1};
        ResponseCallback callback;
        // Order of call(); calls interrupted by a disconnect are sent again in this order.
        quint64 sequence{0};
    };

    // Calls sent before earlier responses arrive. The rest wait in queued_, so a reconnect
    // does not send the whole backlog at once and flood the daemon with responses to write.
    static constexpr int maximumCallsInFlight = 16;
    // A connection that stays up this long ends a reconnect loop; one that drops sooner keeps
    // backing off instead of reconnecting at once after every hello.
    static constexpr int stableConnectionMs = 5'000;

    void initialiseTimers();
    void attachSocket(QLocalSocket* socket);
    void handleDisconnected();
    void abortConnection();
    void connectToDaemon();
    void scheduleReconnect();
    // ~/Library/Logs/KetPlus/daemon.log, rotated once past 8 MiB; empty if it cannot be made.
    [[nodiscard]] static QString daemonLogPath();
    void launchDaemon();
    void authenticate();
    void finishAuthentication();
    // Queues a call, merged into an identical read already waiting to be sent.
    void enqueue(QueuedCall call);
    void sendQueued();
    void replaceIdleDaemon(const QString& daemonVersion);
    void sendCall(QueuedCall call);
    void sendEnvelope(const QJsonObject& envelope);
    void readFrames();
    void consumeFrame(const QByteArray& frame);
    void setReady(bool ready);
    void failPending(const QString& reason);
    [[nodiscard]] static QString daemonExecutable();
    [[nodiscard]] static QByteArray encodeFrame(const QJsonObject& envelope);

    QPointer<QLocalSocket> socket_;
    CredentialSource credential_;
    bool adopted_{false};
    QTimer reconnectTimer_;
    QTimer stableTimer_;
    QByteArray buffer_;
    quint32 expectedSize_{0};
    QHash<QString, QueuedCall> pending_;
    QList<QueuedCall> queued_;
    QString socketPath_;
    QString tokenPath_;
    QString statePath_;
    QString clientId_;
    QStringList features_;
    QStringList capabilities_;
    QString hostKeyFingerprint_;
    QString trustedHostKeyFingerprint_;
    bool identityRejected_{false};
    bool hostIdentityRequired_{false};
    bool activityGroupsEnabled_{false};
    int protocolMinor_{0};
    bool ready_{false};
    bool launched_{false};
    bool replacementAttempted_{false};
    int reconnectAttempt_{0};
    quint64 nextSequence_{0};
};

} // namespace ketplus::remote
