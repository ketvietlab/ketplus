#include "app/MainWindow.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
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

} // namespace

void MainWindow::connectAgentWorkReport() {
    connect(agentWorkspace_, &AgentWorkspaceView::workReportRequested, this,
            &MainWindow::openAgentWorkReportTab);
    // The pane takes only its own conversation's reports, read or live.
    connect(agentManager_, &AgentSessionManager::workReportChanged, this,
            [this](const QString&, const QString& conversationId, const QVariantMap& report,
                   const QString& error) {
                if (auto* pane = workReportPane(agentDetailTabs_))
                    pane->acceptReport(conversationId, report, error);
            });
    // Selecting another conversation or worktree moves the open tab to it.
    connect(agentManager_, &AgentSessionManager::messagesReset, this,
            [this] { refreshAgentWorkReport(false); });
    connect(agentWorkspace_, &AgentWorkspaceView::activePathChanged, this,
            [this] { refreshAgentWorkReport(false); });
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

} // namespace ketplus
