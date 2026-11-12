#include "app/MainWindow.h"

#include "git/GitChangesPanel.h"
#include "machine/MachineComposition.h"
#include "machine/MachineSelection.h"
#include "machine/MachineWorkspaceView.h"
#include "remote/RemoteControlView.h"
#include "terminal/TerminalPanel.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"

#include <QAction>
#include <QSplitter>

#include <utility>

namespace ketplus {

MachineComposition* MainWindow::machineComposition() {
    if (machines_ != nullptr)
        return machines_;
    machines_ = new MachineComposition(accountDaemon(), this);
    connect(machines_, &MachineComposition::sidebarChanged, this, [this] {
        if (selectedMachineId_ != QStringLiteral("local"))
            refreshSidebarRepositories();
    });
    // The selection changes on its own when Connections registers, replaces or removes a
    // backend. The window follows it here; selectMachine() handles the picker.
    connect(machines_, &MachineComposition::machineChanged, this,
            [this](const QString& machineId, const bool remote) {
                if (machineId != selectedMachineId_) {
                    // Connections no longer lists the picked machine: back to This Mac.
                    machines_->awaitingBackend = false;
                    const bool shown = machines_->shown();
                    selectedMachineId_ = machineId;
                    activityBar_->setRemoteMachineSelected(remote);
                    refreshSidebarRepositories();
                    if (shown && !remote) {
                        setMachineWorkspaceVisible(false);
                        setAgentWorkspaceVisible(true);
                    }
                    return;
                }
                if (!remote)
                    return;
                refreshSidebarRepositories();
                // A backend arrived for the picked machine: its workspace shows if it was picked
                // without one, or wherever the window is not on Connections.
                if (machines_->selection()->backend() == nullptr)
                    return;
                if (std::exchange(machines_->awaitingBackend, false) || !remoteWorkspaceMode_)
                    setMachineWorkspaceVisible(true);
            });
    return machines_;
}

void MainWindow::selectMachine(const QString& machineId) {
    if (machineId == selectedMachineId_)
        return;
    selectedMachineId_ = machineId;
    const bool local = machineId == QStringLiteral("local");
    activityBar_->setRemoteMachineSelected(!local);
    MachineSelection* selection = machineComposition()->selection();
    machines_->awaitingBackend = false;
    selection->select(machineId);
    refreshSidebarRepositories();
    if (local) {
        setMachineWorkspaceVisible(false);
        setAgentWorkspaceVisible(true);
    } else if (selection->backend() == nullptr) {
        // No route to this machine yet: Connections connects it. The sidebar stays empty
        // rather than showing This Mac's lists. Opening Connections may register it at once.
        setRemoteWorkspaceVisible(true);
        if (selection->backend() != nullptr)
            setMachineWorkspaceVisible(true);
        else
            machines_->awaitingBackend = selection->machineId() == machineId;
    } else {
        setMachineWorkspaceVisible(true);
    }
}

void MainWindow::setMachineWorkspaceVisible(const bool visible) {
    const bool shown = machines_ != nullptr && machines_->shown();
    if (!visible) {
        if (!shown)
            return;
        machines_->existingView()->hide();
        editorSplit_->show();
        terminalAction_->setEnabled(true);
        return;
    }
    if (shown || selectedMachineId_ == QStringLiteral("local"))
        return;
    // Every other workspace hides first. Showing any of them calls setAgentWorkspaceVisible(false),
    // which hides this one.
    setAgentWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    setRemoteWorkspaceVisible(false);
    hideUtilityWorkspaces();
    MachineWorkspaceView* view = machineComposition()->view(workspaceSplit_);
    if (workspaceSplit_->indexOf(view) < 0)
        workspaceSplit_->addWidget(view);
    if (explorer_ != nullptr)
        explorer_->hide();
    if (gitChanges_ != nullptr)
        gitChanges_->hide();
    if (terminal_ != nullptr)
        terminal_->hide();
    hideSearchPanel();
    editorSplit_->hide();
    terminalAction_->setEnabled(false);
    view->show();
    setShellMode(ActivityBar::Mode::AgentWorkspace);
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(view)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
}

} // namespace ketplus
