#include "app/MainWindow.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/ConversationChangesPane.h"
#include "agent/WorkReportPane.h"

#include <QTabWidget>

namespace ketplus {
namespace {

WorkReportPane* workReportPane(QTabWidget* tabs) {
    if (tabs == nullptr)
        return nullptr;
    for (int index = 0; index < tabs->count(); ++index)
        if (auto* pane = qobject_cast<WorkReportPane*>(tabs->widget(index)))
            return pane;
    return nullptr;
}

ConversationChangesPane* changesPane(QTabWidget* tabs) {
    if (tabs == nullptr)
        return nullptr;
    for (int index = 0; index < tabs->count(); ++index)
        if (auto* pane = qobject_cast<ConversationChangesPane*>(tabs->widget(index)))
            return pane;
    return nullptr;
}

} // namespace

void MainWindow::connectAgentWorkReport() {
    connect(agentWorkspace_, &AgentWorkspaceView::workReportRequested, this,
            &MainWindow::openAgentWorkReportTab);
    connect(agentWorkspace_, &AgentWorkspaceView::changesRequested, this,
            &MainWindow::openAgentChangesTab);
    // The pane takes only its own conversation's reports, read or live.
    connect(agentManager_, &AgentSessionManager::workReportChanged, this,
            [this](const QString&, const QString& conversationId, const QVariantMap& report,
                   const QString& error) {
                if (auto* pane = workReportPane(agentDetailTabs_))
                    pane->acceptReport(conversationId, report, error);
            });
    // The running turn's report closes the live transcript, as the mock's running card.
    connect(agentManager_, &AgentSessionManager::turnWorkReportChanged, this,
            [this](const QString& path, const QString& conversationId, const QVariantMap& report) {
                if (path == agentWorkspace_->activePath() &&
                    conversationId == agentManager_->activeConversationId(path))
                    agentWorkspace_->setTurnWorkReport(report);
            });
    // Selecting another conversation or worktree moves the open tab to it.
    connect(agentManager_, &AgentSessionManager::messagesReset, this, [this] {
        agentWorkspace_->setTurnWorkReport({});
        refreshAgentWorkReport(false);
    });
    connect(agentWorkspace_, &AgentWorkspaceView::activePathChanged, this, [this] {
        agentWorkspace_->setTurnWorkReport({});
        refreshAgentWorkReport(false);
    });
}

void MainWindow::openAgentWorkReportTab() {
    if (!agentWorkspaceMode_)
        return;
    if (compactConversationWindow_)
        setCompactConversationWindow(false);
    auto* tabs = ensureAgentDetailTabs();
    auto* pane = workReportPane(tabs);
    if (pane == nullptr) {
        pane = new WorkReportPane(tabs);
        const int index = tabs->addTab(pane, tr("Report"));
        tabs->setTabToolTip(index, tr("Work report of the selected conversation"));
    }
    tabs->setCurrentWidget(pane);
    updateAgentDetailTabs();
    refreshAgentWorkReport(true);
}

void MainWindow::refreshAgentWorkReport(const bool force) {
    auto* pane = workReportPane(agentDetailTabs_);
    if (pane == nullptr || agentWorkspace_ == nullptr || agentManager_ == nullptr)
        return;
    const QString path = agentWorkspace_->activePath();
    const QString conversation = agentManager_->activeConversationId(path);
    if (!force && conversation == pane->conversationId())
        return;
    pane->showConversation(conversation);
    agentManager_->readWorkReport(path);
}

// The mock's header Changes opens the details pane's Changes tab, not Source Control.
void MainWindow::openAgentChangesTab() {
    if (!agentWorkspaceMode_)
        return;
    if (compactConversationWindow_)
        setCompactConversationWindow(false);
    auto* tabs = ensureAgentDetailTabs();
    auto* pane = changesPane(tabs);
    if (pane == nullptr) {
        pane = new ConversationChangesPane(tabs);
        connect(pane, &ConversationChangesPane::fileDiffRequested, this,
                [this](const QString& path) { openTranscriptFile(path, true); });
        connect(pane, &ConversationChangesPane::sourceControlRequested, this,
                [this] { setSourceControlVisible(true); });
        const int index = tabs->addTab(pane, tr("Changes"));
        tabs->setTabToolTip(index, tr("Uncommitted changes of the selected worktree"));
    }
    refreshAgentChanges();
    tabs->setCurrentWidget(pane);
    updateAgentDetailTabs();
}

void MainWindow::refreshAgentChanges() {
    auto* pane = changesPane(agentDetailTabs_);
    if (pane == nullptr)
        return;
    QStringList paths;
    for (const auto& file : gitSnapshot_.files)
        if (!file.ignored)
            paths.append(file.path);
    pane->setChanges(gitSnapshot_.repositoryRoot, gitSnapshot_.branch, paths);
}

} // namespace ketplus
