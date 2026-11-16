#pragma once

#include "storage/BackgroundTaskLedger.h"

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

namespace ketplus::daemon {
namespace executor {
class NativeJobClient;
}

// Owns execution independently of provider transport. SessionHost validates the
// caller/workspace and uses its existing ApprovalBroker before granting permission.
class ManagedTaskRuntime final : public QObject {
  public:
    using Changed = std::function<void(const QJsonObject&)>;
    using Permission = std::function<void(const QJsonObject&, const QJsonObject&)>;
    using ChildStart = std::function<void(const QJsonObject&, const QJsonObject&)>;
    using ChildStop = std::function<void(const QJsonObject&)>;
    ManagedTaskRuntime(BackgroundTaskLedger ledger, QString directory, QString executor,
                       QObject* parent = nullptr);
    void setCallbacks(Changed changed, Permission permission, ChildStart childStart,
                      ChildStop childStop);
    bool initialize(QString* error);
    QJsonObject start(const BackgroundTaskIntent& intent, const QJsonObject& execution);
    QJsonObject stop(const QString& taskId, const BackgroundTaskOwner& owner);
    void permissionResolved(const QString& taskId, bool granted);
    void cancelOwner(const BackgroundTaskOwner& owner, const QString& reason);
    [[nodiscard]] QJsonObject output(const QString& taskId, const BackgroundTaskOwner& owner,
                                     const QString& cursor = {}) const;
    // Managed children report only after inference, approvals and descendants settle.
    void childChanged(const QString& taskId, const BackgroundTaskChange& change);
    QJsonObject reserveChild(const QString& taskId);

  private:
    struct LiveTask {
        QJsonObject record;
        QJsonObject execution;
        QPointer<executor::NativeJobClient> executor;
        QJsonObject pendingExecutorEvent;
        QString persistenceError;
    };
    bool apply(const QString& taskId, const BackgroundTaskChange& change);
    void pump();
    void startProcess(const QString& taskId);
    void observeProcess(const QString& taskId, const QJsonObject& event);
    void reconcileProcess(const QString& taskId);
    void stopTask(const QString& taskId, const QString& reason);
    void publish(const QString& taskId);
    BackgroundTaskLedger ledger_;
    QString directory_;
    QString executor_;
    QHash<QString, std::shared_ptr<LiveTask>> live_;
    QTimer timer_;
    Changed changed_;
    Permission permission_;
    ChildStart childStart_;
    ChildStop childStop_;
    bool pumping_{false};
};
} // namespace ketplus::daemon
