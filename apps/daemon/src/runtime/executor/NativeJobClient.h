#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <functional>

namespace ketplus::daemon::executor {
// The registry commits directory/lease before start(). A reconnect never replays
// start: it only adopts a verifiable supervisor or attempts identity-safe cleanup.
class NativeJobClient final : public QObject {
  public:
    using CommitHandshake = std::function<bool(const QJsonObject&)>;
    using Event = std::function<void(const QJsonObject&)>;
    NativeJobClient(QString executable, QString directory, QString taskId, QString lease,
                    QObject* parent = nullptr);
    ~NativeJobClient() override;
    bool start(const QJsonObject& execution, qint64 remainingTimeoutMs,
               CommitHandshake commitHandshake, Event event, QString* error);
    bool reconnect(Event event, QString* error);
    void stop(const QString& reason);
    // Call only after the registry has committed the receipt result durably.
    void acknowledge();
    void setRecoveryIdentity(const QJsonObject& identity) { recoveryIdentity_ = identity; }
    [[nodiscard]] QJsonObject receipt(QString* error = nullptr) const;

  private:
    bool validateDirectory(QString* error) const;
    void connectSocket();
    void receive();
    void message(const QJsonObject& message);
    void send(const QJsonObject& message);
    void tick();
    void recoverLostSupervisor();
    void reportLost(bool stopped, const QString& issue);
    QString executable_;
    QString directory_;
    QString taskId_;
    QString lease_;
    QLocalSocket socket_;
    QTimer timer_;
    QElapsedTimer clock_;
    QByteArray input_;
    QJsonObject execution_;
    QJsonObject inheritedEnvironment_;
    QJsonObject lastReceipt_;
    QJsonObject recoveryIdentity_;
    CommitHandshake commitHandshake_;
    Event event_;
    qint64 remainingTimeout_{0};
    qint64 lastConnectionAttempt_{-1000};
    qint64 lastMessage_{0};
    qint64 recoveryStarted_{-1};
    qint64 lastRecoveryAttempt_{-1000};
    bool launchedSupervisor_{false};
    bool sentStart_{false};
    bool authenticated_{false};
    bool finished_{false};
    bool acknowledged_{false};
    QString stopReason_;
    QString recoveryIssue_;
    bool recoveryStopped_{false};
    bool recoveryReported_{false};
};
} // namespace ketplus::daemon::executor
