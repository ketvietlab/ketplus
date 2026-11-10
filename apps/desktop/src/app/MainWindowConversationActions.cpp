#include "app/MainWindow.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentWorktreeDialog.h"
#include "app/ConversationMenu.h"

#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetTextField.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
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
        // The mock's Rename conversation dialog: a Name field, then Cancel and Rename.
        auto* dialog = new ket::Dialog(tr("Rename conversation"), this);
        dialog->setObjectName(QStringLiteral("renameConversationDialog"));
        connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
        auto* field = new ket::TextField;
        field->setObjectName(QStringLiteral("renameConversationField"));
        field->setAccessibleName(tr("Name"));
        field->setText(conversation.value(QStringLiteral("title")).toString());
        dialog->setBody(new ket::FieldRow(tr("Name"), field));
        auto* cancel = dialog->addButton(tr("Cancel"), ket::ButtonVariant::Ghost);
        auto* rename = dialog->addButton(tr("Rename"), ket::ButtonVariant::Primary);
        connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
        connect(rename, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
        connect(field, &QLineEdit::returnPressed, dialog, &ket::Dialog::accept);
        connect(dialog, &ket::Dialog::accepted, this, [this, worktreePath, conversationId, field] {
            if (agentManager_ != nullptr)
                agentManager_->renameConversation(worktreePath, conversationId, field->text());
        });
        dialog->open();
        field->selectAll();
        field->setFocus(Qt::OtherFocusReason);
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
    if (agentDialogOpen(this, QStringLiteral("conversationDeleteDialogCard")))
        return;
    const QString path = worktreePath;
    const QString id = conversationId;
    QStringList owned = agentManager_->conversationOwnedWorktreePaths(path, id);
    const bool hasLinkedWorktree = agentManager_->conversationOwnsWorktree(path, id);
    if (hasLinkedWorktree && owned.isEmpty())
        owned = QStringList{path};
    QString reference;
    for (const QVariant& value : agentManager_->conversations(path)) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("id")).toString() == id) {
            const QString number = conversation.value(QStringLiteral("number")).toString();
            if (!number.isEmpty())
                reference = QStringLiteral("@agent/") + number;
            break;
        }
    }
    auto* form =
        new AgentConversationDeleteDialog(hasLinkedWorktree ? owned : QStringList{}, reference);
    connect(form, &AgentConversationDeleteDialog::archiveRequested, this,
            [this, path, id] { archiveAgentConversation(path, id); });
    connect(form->present(this), &ket::Dialog::accepted, this,
            [this, form, path, id, owned, hasLinkedWorktree] {
                if (agentManager_ == nullptr)
                    return;
                clearPendingWorktreeDeletion();
                if (form->deleteLinkedWorktree()) {
                    if (!hasLinkedWorktree) {
                        auto* notice = new QMessageBox(
                            QMessageBox::Warning, tr("Unable to delete worktree"),
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
                    pendingDeleteWorktreeForce_ = form->forceDeleteWorktree();
                }
                agentManager_->deleteConversation(path, id);
            });
}

} // namespace ketplus
