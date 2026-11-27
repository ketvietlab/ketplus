#pragma once

#include "runtime/executor/NativeProcessIdentity.h"
#include "storage/BackgroundTaskOutput.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSqlDatabase>
#include <QTimer>
#include <memory>

class QSocketNotifier;

namespace ketplus::daemon::executor {
// Who asked the supervisor to stop: the signal and the sender the kernel reported.
struct SupervisorStopSignal final {
    int signal{0}; // 0: none received
    qint64 senderPid{0};
    qint64 senderUid{0};
};
[[nodiscard]] SupervisorStopSignal supervisorStopSignal();

class NativeJobSupervisor final : public QObject {
  public:
    NativeJobSupervisor(QString directory, QString taskId, QString lease,
                        QObject* parent = nullptr);
    ~NativeJobSupervisor() override;
    bool initialize(QString* error);

  private:
    void acceptConnection();
    void receive();
    void command(const QJsonObject& message);
    void send(const QString& event, const QJsonObject& data = {});
    bool persist();
    bool checkLease();
    void renewLease();
    void launch(const QJsonObject& message);
    bool prepareLaunchGate(QString* error);
    void commitLaunchIdentity();
    void closeLaunchGate();
    void drainOutput();
    void refreshKnown();
    void signalKnown(int signal);
    void recordStopSender();
    void requestStop(const QString& reason);
    void tick();
    void linuxTick();
    void finish();
    bool verifyGroupEmpty();

    QString directory_;
    QString taskId_;
    QString lease_;
    QString connectionName_;
    QJsonObject metadata_;
    QLocalServer server_;
    QLocalSocket* client_{nullptr};
    QByteArray input_;
    QByteArray executionHash_;
    QProcess process_;
    QSocketNotifier* launchReady_{nullptr};
    int launchIdentityPipe_[2]{-1, -1};
    int launchGate_[2]{-1, -1};
    QTimer timer_;
    QElapsedTimer clock_;
    QSqlDatabase outputDatabase_;
    std::unique_ptr<BackgroundTaskOutput> output_;
    QHash<qint64, NativeProcessIdentity> known_;
    std::optional<NativeProcessIdentity> root_;
    qint64 lastPulse_{0};
    bool pulsed_{false};
    qint64 maxPingGap_{0};
    qint64 channelLostAt_{-1};
    qint64 deadline_{0};
    qint64 killAt_{0};
    qint64 finishCleanupAt_{0};
    bool authenticated_{false};
    bool launchAttempted_{false};
    bool rootExited_{false};
    bool stopping_{false};
    bool cleanupError_{false};
    bool finalized_{false};
    int exitCode_{-1};
    QString stopReason_;
    QString stopCause_;
    bool leaseOrChannelExpired();
};
} // namespace ketplus::daemon::executor
