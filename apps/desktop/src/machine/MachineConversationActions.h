#pragma once

#include "MachineBackend.h"

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

    void rename(const QString& sessionId, const QString& title);
    void fork(const QString& sessionId);
    void archive(const QString& sessionId);

    // The conversation's menu: Rename…, Fork and Archive, each disabled with a tooltip when
    // the route cannot run it. The caller owns the menu.
    [[nodiscard]] QMenu* menu(const QString& sessionId, QWidget* parent);
    // Asks for the new title, then renames.
    void promptRename(const QString& sessionId, QWidget* parent);

  signals:
    void forked(const QString& machineId, const QString& sessionId, qint64 generation);
    void failed(const QString& error);
    void finished();

  private:
    [[nodiscard]] remote::MachineBackend::Callback done(std::function<void(const QJsonObject&)>);
    bool start(Feature feature, const QString& sessionId, qint64* generation);

    MachineCatalog* catalog_;
    QString lastError_;
};

// The sidebar's menu and rename of a remote conversation row: they act through the remote
// Agent Workspace's actions, which show what failed.
void showMachineConversationMenu(MachineComposition* composition, const QString& sessionId,
                                 QWidget* parent);
void renameMachineConversation(MachineComposition* composition, const QString& sessionId,
                               QWidget* parent);

} // namespace ketplus
