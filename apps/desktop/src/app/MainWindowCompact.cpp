#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"
#include "app/MainWindow.h"
#include <QAction>
#include <QSignalBlocker>
#include <QStatusBar>

namespace ketplus {
void MainWindow::setCompactConversationWindow(const bool enabled) {
    if (enabled == compactConversationWindow_)
        return;
    if (enabled) {
        normalConversationGeometry_ = saveGeometry();
        normalConversationMinimum_ = minimumSize();
        normalConversationSize_ = size();
        compactSidebarVisible_ = activityBar_->isVisible();
        compactStatusVisible_ = statusBar()->isVisible();
        compactDetailsVisible_ = agentDetailPane_ && agentDetailPane_->isVisible();
        if (isFullScreen())
            setFullScreen(false);
        if (isMaximized())
            showNormal();
        // Reuse the same view, session and draft. No session switch or new window.
        if (!agentWorkspaceMode_)
            setAgentWorkspaceVisible(true);
        compactConversationWindow_ = true;
        activityBar_->hide();
        statusBar()->hide();
        if (agentDetailPane_)
            agentDetailPane_->hide();
        setMinimumSize(320, 420);
        const auto zoom = ket::Appearance::instance().zoom();
        resize(qRound(ket::Appearance::scale(480, zoom)),
               qMin(height(), qRound(ket::Appearance::scale(760, zoom))));
    } else {
        compactConversationWindow_ = false;
        setMinimumSize(normalConversationMinimum_);
        activityBar_->setVisible(compactSidebarVisible_);
        statusBar()->setVisible(compactStatusVisible_);
        if (agentDetailPane_)
            agentDetailPane_->setVisible(compactDetailsVisible_);
        restoreGeometry(normalConversationGeometry_);
        if (!isFullScreen() && !isMaximized())
            resize(normalConversationSize_);
    }
    if (agentWorkspace_)
        agentWorkspace_->setCompactWindow(enabled);
    if (compactConversationAction_) {
        const QSignalBlocker blocker(compactConversationAction_);
        compactConversationAction_->setChecked(enabled);
    }
}
} // namespace ketplus
