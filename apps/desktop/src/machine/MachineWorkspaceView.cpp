#include "machine/MachineWorkspaceView.h"

#include "machine/MachineApprovalCards.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationSession.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace ketplus {

namespace {

using State = remote::MachineBackend::ConnectionState;

QString stateText(const MachineCatalog& catalog) {
    switch (catalog.state()) {
    case State::Connected:
        return {};
    case State::Connecting:
        return MachineWorkspaceView::tr("Connecting…");
    case State::Reconnecting:
        return catalog.reason().isEmpty()
                   ? MachineWorkspaceView::tr("Reconnecting…")
                   : MachineWorkspaceView::tr("Reconnecting… %1").arg(catalog.reason());
    case State::Offline:
        return catalog.reason().isEmpty() ? MachineWorkspaceView::tr("This machine is offline.")
                                          : catalog.reason();
    }
    return {};
}

} // namespace

MachineWorkspaceView::MachineWorkspaceView(MachineCatalog* catalog, QWidget* parent)
    : QWidget(parent), catalog_(catalog) {
    setObjectName(QStringLiteral("machineWorkspace"));
    auto* layout = new QVBoxLayout(this);
    banner_ = new QLabel(this);
    banner_->setObjectName(QStringLiteral("machineStateBanner"));
    banner_->setWordWrap(true);
    title_ = new QLabel(this);
    title_->setObjectName(QStringLiteral("machineConversationTitle"));
    worktrees_ = new QComboBox(this);
    worktrees_->setObjectName(QStringLiteral("machineWorktreePicker"));
    transcript_ = new QListWidget(this);
    transcript_->setObjectName(QStringLiteral("machineTranscript"));
    transcript_->setWordWrap(true);
    transcript_->setSelectionMode(QAbstractItemView::NoSelection);
    approvals_ = new MachineApprovalCards(this);
    approvals_->setObjectName(QStringLiteral("machineApprovals"));
    composer_ = new QPlainTextEdit(this);
    composer_->setObjectName(QStringLiteral("machineComposer"));
    composer_->setMaximumHeight(120);
    composerReason_ = new QLabel(this);
    composerReason_->setObjectName(QStringLiteral("machineComposerReason"));
    send_ = new QPushButton(tr("Send"), this);
    send_->setObjectName(QStringLiteral("machineSend"));
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("machineStop"));
    auto* actions = new QHBoxLayout;
    actions->addWidget(composerReason_, 1);
    actions->addWidget(stop_);
    actions->addWidget(send_);
    layout->addWidget(banner_);
    layout->addWidget(title_);
    layout->addWidget(worktrees_);
    layout->addWidget(transcript_, 1);
    layout->addWidget(approvals_);
    layout->addWidget(composer_);
    layout->addLayout(actions);
    connect(send_, &QPushButton::clicked, this, &MachineWorkspaceView::submit);
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (session_ != nullptr)
            session_->stop();
    });
    connect(catalog_, &MachineCatalog::changed, this, &MachineWorkspaceView::catalogChanged);
    catalogChanged();
}

MachineConversationSession* MachineWorkspaceView::session() const { return session_; }

QString MachineWorkspaceView::openSessionId() const {
    return session_ != nullptr ? session_->sessionId() : QString{};
}

void MachineWorkspaceView::catalogChanged() {
    // The selected machine changed or its lists did: restore what that machine last showed.
    const QString machine = catalog_->machineId();
    if (session_ == nullptr || session_->machineId() != machine)
        showSession(lastByMachine_.value(machine));
    else
        render();
}

void MachineWorkspaceView::openConversation(const QString& sessionId) {
    if (session_ != nullptr && session_->machineId() == catalog_->machineId() &&
        session_->sessionId() == sessionId)
        return;
    const remote::MachineConversation row = catalog_->conversation(sessionId);
    bind(sessionId, row.generation, row.running());
}

void MachineWorkspaceView::bind(const QString& sessionId, const qint64 generation,
                                const bool running) {
    const QString machine = catalog_->machineId();
    auto* session =
        new MachineConversationSession(catalog_->backend(), sessionId, generation, this);
    session->setRunning(running);
    if (auto previous = lastByMachine_.value(machine); previous != nullptr)
        previous->deleteLater();
    lastByMachine_.insert(machine, session);
    showSession(session);
    session->load();
    emit conversationOpened(machine, sessionId);
}

void MachineWorkspaceView::startNewConversation(const QString& repositoryId) {
    newConversationRepository_ = repositoryId;
    const QString machine = catalog_->machineId();
    if (auto previous = lastByMachine_.take(machine); previous != nullptr)
        previous->deleteLater();
    showSession(nullptr);
    composer_->setFocus();
}

void MachineWorkspaceView::showSession(MachineConversationSession* session) {
    if (session_ != nullptr)
        disconnect(session_, nullptr, this, nullptr);
    session_ = session;
    if (session_ != nullptr)
        connect(session_, &MachineConversationSession::changed, this,
                &MachineWorkspaceView::render);
    approvals_->setSession(session_);
    render();
}

void MachineWorkspaceView::render() {
    const QString status = stateText(*catalog_);
    banner_->setText(status);
    banner_->setVisible(!status.isEmpty());
    const bool newConversation = session_ == nullptr;
    title_->setText(newConversation ? tr("New conversation")
                                    : catalog_->conversation(session_->sessionId()).title);
    worktrees_->setVisible(newConversation);
    if (newConversation)
        renderWorktrees();

    transcript_->clear();
    if (session_ != nullptr) {
        for (const auto& item : session_->transcript().items()) {
            auto* row = new QListWidgetItem(item.text, transcript_);
            row->setData(Qt::UserRole, static_cast<int>(item.kind));
        }
        transcript_->scrollToBottom();
    }
    approvals_->refresh();

    const bool connected = catalog_->state() == State::Connected;
    const bool canAct = newConversation
                            ? connected && worktrees_->currentData().isValid() && !creating_
                            : session_->canAct();
    // A request the route holds or queued is named, so the composer never looks hung.
    QString reason;
    if (newConversation && creating_)
        reason = !connected ? tr("Waiting for the machine to reconnect. The conversation "
                                 "starts when it does (held for up to 15 minutes).")
                 : createCommand_.isEmpty()
                     ? tr("Starting the conversation…")
                     : tr("Sent. Waiting for the machine to start the conversation…");
    else if (session_ != nullptr && !session_->writeStatus().isEmpty())
        reason = session_->writeStatus();
    else if (!connected)
        reason = status;
    else if (newConversation && !worktrees_->currentData().isValid())
        reason = tr("Choose a worktree on this machine.");
    else if (newConversation)
        reason = createError_;
    else
        reason = session_->lastError();
    composer_->setReadOnly(!canAct);
    composer_->setEnabled(canAct);
    composerReason_->setText(reason);
    send_->setEnabled(canAct);
    stop_->setVisible(session_ != nullptr && session_->running());
    stop_->setEnabled(canAct);
}

void MachineWorkspaceView::renderWorktrees() {
    const QString current = worktrees_->currentData().toString();
    const QSignalBlocker blocker(worktrees_);
    worktrees_->clear();
    auto* model = qobject_cast<QStandardItemModel*>(worktrees_->model());
    for (const auto& repository : catalog_->repositories()) {
        if (!newConversationRepository_.isEmpty() && repository.id != newConversationRepository_)
            continue;
        for (const auto& worktree : repository.worktrees) {
            QString label = repository.label + QStringLiteral(" · ") + worktree.label;
            if (!worktree.branch.isEmpty())
                label += QStringLiteral(" (") + worktree.branch + QLatin1Char(')');
            worktrees_->addItem(label, worktree.id);
            worktrees_->setItemData(worktrees_->count() - 1, repository.id, Qt::UserRole + 1);
            if (!worktree.available && model != nullptr)
                model->item(worktrees_->count() - 1)->setEnabled(false);
        }
    }
    // Pending worktrees are listed but not chosen until the route verified them.
    for (const auto& worktree : catalog_->pendingWorktrees()) {
        worktrees_->addItem(tr("%1 · verifying…").arg(worktree.label));
        if (model != nullptr) {
            model->item(worktrees_->count() - 1)->setEnabled(false);
            model->item(worktrees_->count() - 1)->setData(true, Qt::UserRole + 2);
        }
    }
    int index = worktrees_->findData(current);
    if (index < 0)
        for (int i = 0; i < worktrees_->count() && index < 0; ++i)
            if (model == nullptr || model->item(i)->isEnabled())
                index = i;
    worktrees_->setCurrentIndex(index);
}

void MachineWorkspaceView::submit() {
    const QString prompt = composer_->toPlainText().trimmed();
    if (prompt.isEmpty())
        return;
    if (session_ != nullptr) {
        if (!session_->canAct())
            return;
        session_->send(prompt);
        composer_->clear();
        return;
    }
    createConversation(prompt);
}

void MachineWorkspaceView::createConversation(const QString& prompt) {
    auto* backend = catalog_->backend();
    const QVariant worktree = worktrees_->currentData();
    if (backend == nullptr || !worktree.isValid() || creating_ ||
        catalog_->state() != State::Connected)
        return;
    creating_ = true;
    render();
    const QJsonObject request{
        {QStringLiteral("worktree_id"), worktree.toString()},
        {QStringLiteral("repository_id"), worktrees_->currentData(Qt::UserRole + 1).toString()},
        {QStringLiteral("prompt"), prompt}};
    const QString machine = backend->machineId();
    createError_.clear();
    createCommand_.clear();
    disconnect(createCompletion_);
    // The Relay may queue the command; its outcome then arrives through commandCompleted().
    createCompletion_ = connect(
        backend, &remote::MachineBackend::commandCompleted, this,
        [this, machine](const QString& commandId, const QJsonObject& result, const QString& error) {
            if (!createCommand_.isEmpty() && commandId == createCommand_)
                finishCreate(machine, result, error);
        });
    backend->createSession(
        request, [self = QPointer(this), machine](const QJsonObject& result, const QString& error) {
            if (self == nullptr)
                return;
            const QString commandId = remote::queuedCommandId(result);
            if (error.isEmpty() && !commandId.isEmpty()) {
                self->createCommand_ = commandId;
                self->render();
                return;
            }
            self->finishCreate(machine, result, error);
        });
}

void MachineWorkspaceView::finishCreate(const QString& machine, const QJsonObject& result,
                                        const QString& error) {
    creating_ = false;
    createCommand_.clear();
    disconnect(createCompletion_);
    const auto created = remote::createdSession(result);
    // A machine switched away from meanwhile keeps its conversation; it lists on return.
    if (catalog_->machineId() != machine) {
        render();
        return;
    }
    if (!error.isEmpty() || created.sessionId.isEmpty()) {
        createError_ = error.isEmpty() ? tr("The machine did not start the conversation.") : error;
        render();
        return;
    }
    composer_->clear();
    catalog_->refresh();
    bind(created.sessionId, created.generation, true);
}

} // namespace ketplus
