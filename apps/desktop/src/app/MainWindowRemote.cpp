#include "app/MainWindow.h"

#include "git/GitChangesPanel.h"
#include "machine/MachineComposition.h"
#include "machine/MachineSelection.h"
#include "remote/LinuxMachines.h"
#include "remote/LinuxMachinesSection.h"
#include "remote/RemoteControlView.h"
#include "terminal/TerminalPanel.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"

#include <QAction>
#include <QSignalBlocker>
#include <QSplitter>

namespace ketplus {

void MainWindow::setRemoteWorkspaceVisible(const bool visible) {
    if (!visible) {
        if (!remoteWorkspaceMode_)
            return;
        remoteWorkspaceMode_ = false;
        if (remoteControl_ != nullptr)
            remoteControl_->hide();
        editorSplit_->show();
        terminalAction_->setEnabled(true);
        const QSignalBlocker blocker(remoteControlAction_);
        remoteControlAction_->setChecked(false);
        return;
    }

    setAgentWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    ensureRemoteControl();
    if (explorer_ != nullptr)
        explorer_->hide();
    if (gitChanges_ != nullptr)
        gitChanges_->hide();
    if (terminal_ != nullptr)
        terminal_->hide();
    hideSearchPanel();
    editorSplit_->hide();
    remoteWorkspaceMode_ = true;
    remoteControl_->show();
    remoteControl_->activate();
    const QSignalBlocker remoteBlocker(remoteControlAction_);
    const QSignalBlocker explorerBlocker(explorerAction_);
    const QSignalBlocker sourceBlocker(sourceControlAction_);
    const QSignalBlocker terminalBlocker(terminalAction_);
    remoteControlAction_->setChecked(true);
    explorerAction_->setChecked(false);
    sourceControlAction_->setChecked(false);
    terminalAction_->setChecked(false);
    terminalAction_->setEnabled(false);
    setShellMode(ActivityBar::Mode::Remote);
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(remoteControl_)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
}

void MainWindow::ensureRemoteControl() {
    if (remoteControl_ != nullptr)
        return;
    remoteControl_ = new RemoteControlView(workspaceSplit_);
    remoteControl_->hide(); // shown only by setRemoteWorkspaceVisible
    connect(remoteControl_, &RemoteControlView::machinesChanged, this,
            [this](const QStringList& ids, const QStringList& names) {
                remoteMachineIds_ = ids;
                QStringList choices{QStringLiteral("local")};
                choices.append(ids);
                QStringList labels{tr("This Mac")};
                labels.append(names);
                // ADR 0119: every listed machine's backend, SSH or the Relay fallback. A
                // machine no longer listed is forgotten (MainWindowMachine follows it).
                auto* section = remoteControl_->linuxMachines();
                machineComposition()->selection()->syncBackends(
                    ids, [section](const QString& id) { return section->machineBackend(id); });
                activityBar_->setMachines(choices, labels, selectedMachineId_);
                activityBar_->setRemoteMachineSelected(selectedMachineId_ !=
                                                       QStringLiteral("local"));
                refreshSidebarRepositories();
            });
    connect(remoteControl_, &RemoteControlView::accountChanged, activityBar_,
            &ActivityBar::setAccount); // sign-in and sign-out happen here
    workspaceSplit_->addWidget(remoteControl_);
    if (sharedLinuxMachines_ != nullptr) {
        remoteControl_->linuxMachines()->setMachines(sharedLinuxMachines_);
        return;
    }
    // No shared set: this window's own saved SSH machines and Relay route (ADR 0119).
    remoteControl_->linuxMachines()->attachSshBackend();
    remoteControl_->linuxMachines()->attachRelayBackend();
}

void MainWindow::useLinuxMachines(LinuxMachines* machines) {
    sharedLinuxMachines_ = machines;
    ensureRemoteControl();
    remoteControl_->linuxMachines()->setMachines(machines);
}

} // namespace ketplus
