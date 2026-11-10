#include "app/MainWindow.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentWorktreeDialog.h"
#include "app/ConversationMenu.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>

namespace ketplus {

void MainWindow::changeAgentConversationWorktree(const QString& path, const QString& id) {
    openSidebarConversation(path, id);
    if (agentWorkspace_ && agentManager_ && agentWorkspace_->activePath() == path &&
        agentManager_->activeConversationId(path) == id)
        agentWorkspace_->requestExecutionWorktree();
}

void MainWindow::renameAgentConversation(const QString& worktreePath,
                                         const QString& conversationId) {
    if (agentManager_ == nullptr || conversationId.isEmpty())
        return;
    for (const QVariant& value : agentManager_->conversations(worktreePath)) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("id")).toString() != conversationId)
            continue;
        bool accepted = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("Rename Conversation"), QStringLiteral("Name:"), QLineEdit::Normal,
            conversation.value(QStringLiteral("title")).toString(), &accepted);
        if (accepted)
            agentManager_->renameConversation(worktreePath, conversationId, title);
        return;
    }
}

// Conversation actions take the conversation's own worktree: the sidebar lists conversations of
// every worktree and repository, not only the one open in this window.
void MainWindow::showAgentConversationContextMenu(const QString& worktreePath,
                                                  const QString& conversationId) {
    if (agentManager_ == nullptr || conversationId.isEmpty())
        return;
    const QString path = worktreePath;
    bool known = false;
    bool running = false;
    bool forkable = false;
    QString number;
    for (const QVariant& value : agentManager_->conversations(path)) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("id")).toString() != conversationId)
            continue;
        known = true;
        running = conversation.value(QStringLiteral("running")).toBool();
        forkable = conversation.value(QStringLiteral("forkable")).toBool();
        number = conversation.value(QStringLiteral("number")).toString();
        break;
    }
    if (!known)
        return;

    QMenu menu(this);
    const QString reference = number.isEmpty() ? QString{} : QStringLiteral("@agent/") + number;
    const auto actions = addConversationMenuActions(menu, forkable, running, reference);
    QAction* selected = menu.exec(QCursor::pos());
    if (selected == actions.rename)
        renameAgentConversation(path, conversationId);
    else if (selected == actions.changeWorktree)
        changeAgentConversationWorktree(path, conversationId);
    else if (selected == actions.fork)
        forkAgentConversation(path, conversationId);
    else if (selected == actions.copyReference) {
        QGuiApplication::clipboard()->setText(reference);
        statusBar()->showMessage(tr("Copied %1").arg(reference), 3000);
    } else if (selected == actions.archive)
        archiveAgentConversation(path, conversationId);
    else if (selected == actions.remove)
        deleteAgentConversation(path, conversationId);
}

void MainWindow::forkAgentConversation(const QString& worktreePath, const QString& conversationId) {
    if (agentManager_ == nullptr || conversationId.isEmpty())
        return;
    statusBar()->showMessage(tr("Forking the conversation…"));
    agentManager_->forkConversation(worktreePath, conversationId);
}

void MainWindow::archiveAgentConversation(const QString& worktreePath,
                                          const QString& conversationId) {
    if (agentManager_ == nullptr || conversationId.isEmpty())
        return;
    if (!agentManager_->archiveConversation(worktreePath, conversationId)) {
        statusBar()->showMessage(QStringLiteral("The conversation could not be archived."), 5000);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Conversation archived. Restore it from Settings."),
                             5000);
}

void MainWindow::deleteAgentConversation(const QString& worktreePath,
                                         const QString& conversationId) {
    if (agentManager_ == nullptr || conversationId.isEmpty())
        return;
    const QString path = worktreePath;
    const QString id = conversationId;
    QStringList owned = agentManager_->conversationOwnedWorktreePaths(path, id);
    const bool hasLinkedWorktree = agentManager_->conversationOwnsWorktree(path, id);
    if (hasLinkedWorktree && owned.isEmpty())
        owned = QStringList{path};
    AgentConversationDeleteDialog dialog(hasLinkedWorktree ? owned : QStringList{}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    clearPendingWorktreeDeletion();
    if (dialog.deleteLinkedWorktree()) {
        if (!hasLinkedWorktree) {
            auto* notice =
                new QMessageBox(QMessageBox::Warning, tr("Unable to delete worktree"),
                                tr("This conversation is not linked to a dedicated worktree."),
                                QMessageBox::Ok, this);
            notice->setObjectName(QStringLiteral("worktreeDeletionError"));
            notice->setAttribute(Qt::WA_DeleteOnClose);
            notice->open();
            return;
        }
        pendingDeleteConversationPath_ = path;
        pendingDeleteWorktreePaths_ = owned;
        pendingDeleteWorktreeRepository_ = primaryWorktreePath();
        pendingDeleteWorktreeForce_ = dialog.forceDeleteWorktree();
    }
    agentManager_->deleteConversation(path, id);
}

} // namespace ketplus
