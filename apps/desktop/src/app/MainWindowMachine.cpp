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

namespace ketplus {

MachineComposition* MainWindow::machineComposition() {
    if (machines_ != nullptr)
        return machines_;
    machines_ = new MachineComposition(accountDaemon(), this);
    connect(machines_, &MachineComposition::sidebarChanged, this, [this] {
        if (selectedMachineId_ != QStringLiteral("local"))
            refreshSidebarRepositories();
    });
    connect(machines_, &MachineComposition::machineChanged, this,
            [this](const QString& machineId, const bool remote) {
                // A backend registered for the picked machine, or the picked machine removed:
                // the window follows the selection.
                if (remote && machineId == selectedMachineId_) {
                    setMachineWorkspaceVisible(true);
                    refreshSidebarRepositories();
                } else if (!remote && selectedMachineId_ != QStringLiteral("local") &&
                           machines_->selection()->backendFor(selectedMachineId_) == nullptr &&
                           machineWorkspaceMode_) {
                    selectMachine(QStringLiteral("local"));
                }
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
    if (local) {
        selection->select(machineId);
        setMachineWorkspaceVisible(false);
        refreshSidebarRepositories();
        setAgentWorkspaceVisible(true);
        return;
    }
    if (selection->backendFor(machineId) == nullptr) {
        // No route to this machine yet: Connections connects it. Its lists stay empty rather
        // than showing This Mac's.
        selection->select(QStringLiteral("local"));
        activityBar_->setRepositories({});
        setRemoteWorkspaceVisible(true);
        return;
    }
    selection->select(machineId);
    refreshSidebarRepositories();
    setMachineWorkspaceVisible(true);
}

void MainWindow::setMachineWorkspaceVisible(const bool visible) {
    if (!visible) {
        if (!machineWorkspaceMode_)
            return;
        machineWorkspaceMode_ = false;
        if (machines_ != nullptr && machines_->existingView() != nullptr)
            machines_->existingView()->hide();
        editorSplit_->show();
        terminalAction_->setEnabled(true);
        return;
    }
    if (machineWorkspaceMode_ || selectedMachineId_ == QStringLiteral("local") ||
        machineComposition()->selection()->backendFor(selectedMachineId_) == nullptr)
        return;
    // Every other workspace hides first; hideUtilityWorkspaces() also hides this one.
    setAgentWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    setRemoteWorkspaceVisible(false);
    hideUtilityWorkspaces();
    MachineWorkspaceView* view = machines_->view(workspaceSplit_);
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
    machineWorkspaceMode_ = true;
    view->show();
    setShellMode(ActivityBar::Mode::AgentWorkspace);
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(view)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
}

} // namespace ketplus
