#pragma once

#include "workspace/SidebarRepositoryList.h"

#include <QObject>
#include <QPointer>

class QWidget;

namespace ketplus::remote {
class DaemonClient;
class DaemonMachineBackend;
} // namespace ketplus::remote

namespace ketplus {

class MachineCatalog;
class MachineSelection;
class MachineWorkspaceView;

// ADR 0119: what one window composes from the selected machine. This Mac keeps its existing
// Agent Workspace and sidebar; any other selected machine is shown through its MachineBackend:
// the catalog feeds the sidebar and the worktree picker, and MachineWorkspaceView the transcript,
// composer and approvals. A remote selection never falls back to This Mac's daemon: a machine
// that is not connected shows its state and reason instead.
class MachineComposition final : public QObject {
    Q_OBJECT

  public:
    MachineComposition(remote::DaemonClient* localDaemon, QObject* parent = nullptr);
    ~MachineComposition() override;

    [[nodiscard]] MachineSelection* selection() const { return selection_; }
    [[nodiscard]] MachineCatalog* catalog() const { return catalog_; }
    [[nodiscard]] bool remoteSelected() const;
    // The Agent Workspace of the selected remote machine, created on first use.
    [[nodiscard]] MachineWorkspaceView* view(QWidget* parent);
    [[nodiscard]] MachineWorkspaceView* existingView() const;
    // The remote Agent Workspace is the window's current workspace.
    [[nodiscard]] bool shown() const;
    // The sidebar rows of the selected remote machine.
    [[nodiscard]] QVector<SidebarRepository> sidebarRepositories() const;

  signals:
    // The selected machine changed (`remote` false for This Mac).
    void machineChanged(const QString& machineId, bool remote);
    // The selected remote machine's sidebar rows changed.
    void sidebarChanged();

  private:
    remote::DaemonMachineBackend* local_{nullptr};
    MachineSelection* selection_{nullptr};
    MachineCatalog* catalog_{nullptr};
    QPointer<MachineWorkspaceView> view_;
};

} // namespace ketplus
