#pragma once

#include "runtime/ManagedTaskRuntime.h"

#include <QTemporaryDir>

namespace ketplus::daemon {
class SessionHost;
class ManagedTaskBridge final : public QObject {
  public:
    using Done = std::function<void(const QJsonObject&)>;
    explicit ManagedTaskBridge(SessionHost* host);
    bool initialize(QString* error);
    void command(const QJsonObject& request, const QString& clientId, Done done, bool agentCaller);
    bool permission(const QJsonValue& requestId, const QJsonObject& result);
    void hold(const BackgroundTaskOwner& owner, const QString& reason);
    void resume(const BackgroundTaskOwner& owner);
    QJsonObject prepareTurn(const BackgroundTaskOwner& owner);
    [[nodiscard]] bool hasExecutions() const;
    [[nodiscard]] QJsonObject snapshot(const BackgroundTaskOwner& owner);
    void childOutput(const QString& sessionId, qint64 generation, qint64 turn, const QString& text);

  private:
    void start(const QJsonObject& request, const BackgroundTaskOwner& owner, Done done);
    void requestPermission(const QJsonObject& task, const QJsonObject& execution);
    void changed(const QJsonObject& task);
    void createChild(const QJsonObject& task, const QJsonObject& execution);
    void stopChild(const QJsonObject& task);
    void poll();
    void pollChildren();
    void recordChildEvent(const QJsonObject& event);
    void dispatchWakes();
    [[nodiscard]] bool eligible(const BackgroundTaskOwner& owner, bool idle) const;
    SessionHost* host_;
    BackgroundTaskLedger ledger_;
    std::unique_ptr<ManagedTaskRuntime> runtime_;
    std::unique_ptr<QTemporaryDir> memoryDirectory_;
    QHash<QString, QPair<QString, QString>> permissions_; // private route -> (task, approval)
    QTimer timer_;
    QString error_;
    qint64 outboxAfter_{0};
    qint64 wakeAfter_{0};
    QHash<QString, QString> stoppingChildren_;
    struct Wake {
        QString eventId;
        qint64 generation;
        qint64 turn;
    };
    QHash<QString, Wake> activeWakes_;
    QHash<QString, QList<QByteArray>> streamedItems_;
    bool deliveryPending_{false};
};
} // namespace ketplus::daemon
