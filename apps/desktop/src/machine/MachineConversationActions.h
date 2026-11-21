#pragma once

#include "MachineBackend.h"

#include <QHash>
#include <QObject>
#include <QString>

class QMenu;
class QWidget;

namespace ketplus {

class MachineCatalog;
class MachineComposition;

// ADR 0119: rename, fork and archive of a conversation on the selected remote machine, through
// its MachineBackend. An action the route does not carry is offered disabled with the route's
// reason as its tooltip (MachineBackend::unsupportedReason()), never hidden; a failure is kept
// in lastError() and reported by failed(), never dropped.
class MachineConversationActions final : public QObject {
    Q_OBJECT

  public:
    using Feature = remote::MachineBackend::Feature;

    explicit MachineConversationActions(MachineCatalog* catalog, QObject* parent = nullptr);

    // Whether the selected machine can run `feature` now; when not, `reason` says why.
    [[nodiscard]] bool available(Feature feature, QString* reason = nullptr) const;
    [[nodiscard]] QString lastError() const { return lastError_; }
    // What the latest command for `sessionId` on the selected machine waits for (the Relay
    // queues commands until the host answers); empty when nothing waits.
    [[nodiscard]] QString pendingStatus(const QString& sessionId) const;

    void rename(const QString& sessionId, const QString& title);
    void fork(const QString& sessionId);
    void archive(const QString& sessionId);

    // The conversation's menu: Rename…, Fork and Archive, each disabled with a tooltip when
    // the route cannot run it. The caller owns the menu.
    [[nodiscard]] QMenu* menu(const QString& sessionId, QWidget* parent);
    // Asks for the new title, then renames.
    void promptRename(const QString& sessionId, QWidget* parent);

  signals:
    // A fork of `sourceSessionId` was asked for; `requestId` names it until forked() or
    // forkFailed() reports it.
    void forkRequested(const QString& machineId, const QString& requestId,
                       const QString& sourceSessionId);
    void forked(const QString& machineId, const QString& requestId, const QString& sessionId,
                qint64 generation);
    void forkFailed(const QString& requestId);
    void failed(const QString& error);
    void finished();
    void pendingChanged();

  private:
    [[nodiscard]] remote::MachineBackend::Callback
    done(const QString& sessionId, const QString& sent,
         std::function<void(const QJsonObject&)> succeeded = {},
         std::function<void()> failedToo = {});
    bool start(Feature feature, const QString& sessionId, qint64* generation);

    MachineCatalog* catalog_;
    QString lastError_;
    struct Pending {
        QString machineId;
        QString sessionId;
        QString status;
        quint64 order{0};
    };
    quint64 nextOrder_{0};
    QHash<QString, Pending> pending_; // queued command id -> its machine and what it waits for
};

// The sidebar's menu and rename of a remote conversation row: they act through the remote
// Agent Workspace's actions, which show what failed.
void showMachineConversationMenu(MachineComposition* composition, const QString& sessionId,
                                 QWidget* parent);
void renameMachineConversation(MachineComposition* composition, const QString& sessionId,
                               QWidget* parent);

} // namespace ketplus
