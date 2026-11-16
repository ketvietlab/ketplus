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

namespace ketplus::daemon::executor {
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
    void launch(const QJsonObject& message);
    void drainOutput();
    void refreshKnown();
    void signalKnown(int signal);
    void requestStop(const QString& reason);
    void tick();
    void finish();

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
    QTimer timer_;
    QElapsedTimer clock_;
    QSqlDatabase outputDatabase_;
    std::unique_ptr<BackgroundTaskOutput> output_;
    QHash<qint64, NativeProcessIdentity> known_;
    std::optional<NativeProcessIdentity> root_;
    qint64 lastPulse_{0};
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
};
} // namespace ketplus::daemon::executor
