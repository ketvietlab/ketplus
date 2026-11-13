#include "machine/MachineWorkspaceView.h"

#include "MachineBackend.h"
#include "MachineRows.h"
#include "machine/MachineAttachmentPanel.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationActions.h"
#include "machine/MachineConversationSession.h"
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <memory>
#include <utility>

namespace ketplus {

void MachineWorkspaceView::renderActions(const bool newConversation) {
    more_->setVisible(!newConversation);
    // Attachments are offered on every route and say why they cannot be used yet.
    QString reason;
    auto* backend = catalog_->backend();
    const bool prepare = newConversation && backend &&
                         backend->supports(remote::MachineBackend::Feature::SessionPreparation) &&
                         worktrees_->currentData().isValid() && !creating_ &&
                         (!draftSession_ || !draftSession_->hasAttachmentSend());
    attach_->setEnabled(
        prepare || (session_ && actions_->available(
                                    MachineConversationActions::Feature::Attachments, &reason)));
    attach_->setText(tr("Attach"));
    attach_->setToolTip(
        session_  ? reason
        : prepare ? QString{}
                  : tr("This machine does not offer attachments before its first message yet."));
}

void MachineWorkspaceView::renderPendingAttachments() {
    while (auto* item = pendingAttachmentLayout_->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    pendingAttachments_->setVisible(!session_ && !pendingAttachmentPaths_.isEmpty());
    if (session_ || pendingAttachmentPaths_.isEmpty())
        return;
    for (const auto& path : std::as_const(pendingAttachmentPaths_)) {
        auto* row = new QWidget(pendingAttachments_);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* name = new QLabel(QFileInfo(path).fileName(), row);
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        auto* remove = new QPushButton(tr("Remove"), row);
        remove->setAccessibleName(tr("Remove %1").arg(QFileInfo(path).fileName()));
        connect(remove, &QPushButton::clicked, this, [this, path] {
            pendingAttachmentPaths_.removeAll(path);
            if (pendingAttachmentPaths_.isEmpty())
                discardPreparation(); // Any late result is cancelled by the epoch guard.
            render();
        });
        line->addWidget(name, 1);
        line->addWidget(remove);
        pendingAttachmentLayout_->addWidget(row);
    }
    if (!preparing_) {
        auto* retry = new QPushButton(tr("Retry attachments"), pendingAttachments_);
        retry->setObjectName("machineRetryAttachments");
        connect(retry, &QPushButton::clicked, this, [this] { attachFiles({}); });
        pendingAttachmentLayout_->addWidget(retry, 0, Qt::AlignLeft);
    }
}

void MachineWorkspaceView::discardPreparation() {
    ++draftEpoch_;
    preparing_ = false;
    pendingAttachmentPaths_.clear();
    draftRequestId_.clear();
    if (draftSession_ && draftBackend_)
        draftBackend_->cancelPreparedSession(draftSession_->sessionId(),
                                             draftSession_->generation(),
                                             [](const QJsonObject&, const QString&) {});
    if (draftPanel_) {
        if (attachmentPanel_ == draftPanel_)
            attachmentPanel_ = nullptr;
        attachmentStack_->removeWidget(draftPanel_);
        draftPanel_->deleteLater();
    }
    if (draftSession_)
        draftSession_->deleteLater();
    draftPanel_.clear();
    draftSession_.clear();
    draftBackend_.clear();
}

void MachineWorkspaceView::attachFiles(const QStringList& paths) {
    if (session_ || draftPanel_) {
        if (attachmentPanel_)
            attachmentPanel_->addFiles(paths);
        return;
    }
    auto* backend = catalog_->backend();
    if (!backend || creating_ || !worktrees_->currentData().isValid() ||
        !backend->supports(remote::MachineBackend::Feature::SessionPreparation) ||
        (paths.isEmpty() && pendingAttachmentPaths_.isEmpty()))
        return;
    for (const auto& path : paths)
        if (!pendingAttachmentPaths_.contains(path))
            pendingAttachmentPaths_.append(path);
    if (preparing_) {
        render();
        return;
    }
    if (draftRequestId_.isEmpty()) {
        draftRequestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        draftWorktree_ = worktrees_->currentData().toString();
        draftRepository_ = worktrees_->currentData(Qt::UserRole + 1).toString();
    }
    draftBackend_ = backend;
    preparing_ = true;
    createError_.clear();
    render();

    // The operation belongs to the route, not the view. If the reader leaves while a
    // durable Relay command is queued, its eventual signed result still discards the
    // unused allocation. No stale result may attach files to a different composer.
    auto* operation = new QObject(backend);
    const QPointer<QObject> operationGuard(operation);
    const QPointer<remote::MachineBackend> route(backend);
    const auto epoch = draftEpoch_;
    const auto finish = [self = QPointer(this), operationGuard, route,
                         epoch](const QJsonObject& result, const QString& error) {
        if (!operationGuard || operationGuard->property("finished").toBool())
            return;
        const auto queued = remote::queuedCommandId(result);
        if (error.isEmpty() && !queued.isEmpty()) {
            operationGuard->setProperty("command", queued);
            return;
        }
        operationGuard->setProperty("finished", true);
        operationGuard->deleteLater();
        const auto prepared = remote::createdSession(result);
        if (!self || self->draftEpoch_ != epoch || self->catalog_->backend() != route) {
            if (route && !prepared.sessionId.isEmpty())
                route->cancelPreparedSession(prepared.sessionId, prepared.generation,
                                             [](const QJsonObject&, const QString&) {});
            return;
        }
        self->preparing_ = false;
        if (!error.isEmpty() || !result.value("ok").toBool() || prepared.sessionId.isEmpty() ||
            prepared.generation != 1 ||
            result.value("expires_at_ms").toInteger() <= QDateTime::currentMSecsSinceEpoch()) {
            self->createError_ =
                !error.isEmpty() ? error
                                 : result.value("error_message")
                                       .toString(tr("The machine could not prepare attachments."));
            self->render();
            return;
        }
        self->preparedAttachments(result);
    };
    connect(backend, &remote::MachineBackend::commandCompleted, operation,
            [operationGuard, finish](const QString& command, const QJsonObject& result,
                                     const QString& error) {
                if (operationGuard && !operationGuard->property("command").toString().isEmpty() &&
                    command == operationGuard->property("command").toString())
                    finish(result, error);
            });
    // Beyond the fixed daemon TTL there is no allocation left to cancel.
    QTimer::singleShot(24 * 60 * 60 * 1000 + 60000, operation, [finish] {
        finish({}, tr("Attachment preparation expired. Please retry."));
    });
    backend->prepareSession({{"worktree_id", draftWorktree_},
                             {"repository_id", draftRepository_},
                             {"client_request_id", draftRequestId_}},
                            finish);
}

void MachineWorkspaceView::preparedAttachments(const QJsonObject& result) {
    const auto prepared = remote::createdSession(result);
    draftSession_ = new MachineConversationSession(draftBackend_, prepared.sessionId,
                                                   prepared.generation, this);
    draftPanel_ = new MachineAttachmentPanel(draftBackend_, prepared.sessionId, prepared.generation,
                                             attachmentStack_);
    attachmentStack_->addWidget(draftPanel_);
    attachmentStack_->setCurrentWidget(draftPanel_);
    attachmentPanel_ = draftPanel_;
    attachmentStack_->show();
    connect(draftPanel_, &MachineAttachmentPanel::changed, this, &MachineWorkspaceView::render);
    connect(draftSession_, &MachineConversationSession::changed, this,
            &MachineWorkspaceView::render);
    const auto paths = std::exchange(pendingAttachmentPaths_, {});
    draftPanel_->addFiles(paths);
    render();
}

void MachineWorkspaceView::sendPreparedAttachments(const QString& prompt) {
    if (!draftSession_ || !draftPanel_ || !draftPanel_->allReady() || !draftSession_->canAct())
        return;
    if (!draftPanel_->hasAttachments()) {
        discardPreparation();
        createConversation(prompt);
        return;
    }
    const auto epoch = draftEpoch_;
    auto* pending = draftSession_.data();
    const auto connection = std::make_shared<QMetaObject::Connection>();
    *connection =
        connect(pending, &MachineConversationSession::attachmentsSent, this,
                [this, epoch, pending, connection](const QJsonArray&, const QString& error) {
                    disconnect(*connection);
                    if (epoch != draftEpoch_ || pending != draftSession_)
                        return;
                    draftPanel_->sent(error.isEmpty());
                    if (!error.isEmpty()) {
                        createError_ = error;
                        render();
                        return;
                    }
                    composer_->clear();
                    // Promote only after the first real turn succeeds. No transcript read or
                    // sidebar conversation exists for the inactive attachment scope.
                    const auto id = pending->sessionId();
                    const auto generation = pending->generation();
                    auto* consumedPanel = draftPanel_.data();
                    ++draftEpoch_;
                    draftSession_.clear();
                    draftPanel_.clear();
                    draftRequestId_.clear();
                    draftBackend_.clear();
                    pending->deleteLater();
                    attachmentPanel_ = nullptr;
                    attachmentStack_->removeWidget(consumedPanel);
                    consumedPanel->deleteLater();
                    bind(id, generation, true);
                    catalog_->refresh();
                    emit conversationOpened(catalog_->machineId(), id);
                });
    const auto attachments = draftPanel_->attachmentIds();
    draftPanel_->setSending(true);
    pending->send(prompt, attachments);
}

} // namespace ketplus
