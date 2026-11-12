#include "machine/MachineWorkspaceView.h"

#include "machine/MachineApprovalCards.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineConversationActions.h"
#include "machine/MachineConversationSession.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QToolButton>
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
    actions_ = new MachineConversationActions(catalog_, this);
    more_ = new QToolButton(this);
    more_->setObjectName(QStringLiteral("machineConversationActions"));
    more_->setText(QStringLiteral("…"));
    more_->setToolTip(tr("Rename, fork or archive"));
    auto* heading = new QHBoxLayout;
    heading->addWidget(title_, 1);
    heading->addWidget(more_);
    // ADR 0119: remote conversations keep this simplified transcript until the rich renderer
    // reads MachineBackend; the note says so instead of passing it off as the full view.
    transcriptNote_ = new QLabel(tr("Simplified transcript: rich rendering of conversations on "
                                    "Linux machines is not available yet."),
                                 this);
    transcriptNote_->setObjectName(QStringLiteral("machineTranscriptNote"));
    transcriptNote_->setWordWrap(true);
    older_ = new QPushButton(tr("Show earlier messages"), this);
    older_->setObjectName(QStringLiteral("machineLoadOlder"));
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
    attach_ = new QPushButton(tr("Attach"), this);
    attach_->setObjectName(QStringLiteral("machineAttach"));
    send_ = new QPushButton(tr("Send"), this);
    send_->setObjectName(QStringLiteral("machineSend"));
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("machineStop"));
    auto* actions = new QHBoxLayout;
    actions->addWidget(composerReason_, 1);
    actions->addWidget(attach_);
    actions->addWidget(stop_);
    actions->addWidget(send_);
    layout->addWidget(banner_);
    layout->addLayout(heading);
    layout->addWidget(worktrees_);
    layout->addWidget(transcriptNote_);
    layout->addWidget(older_);
    layout->addWidget(transcript_, 1);
    layout->addWidget(approvals_);
    layout->addWidget(composer_);
    layout->addLayout(actions);
    connect(send_, &QPushButton::clicked, this, &MachineWorkspaceView::submit);
    connect(stop_, &QPushButton::clicked, this, [this] {
        if (session_ != nullptr)
            session_->stop();
    });
    connect(older_, &QPushButton::clicked, this, [this] {
        if (session_ != nullptr)
            session_->loadOlder();
    });
    // The reader scrolling to the top reads the page before it, as This Mac's transcript does.
    // Only a user action counts: re-rendering the rows resets the bar and must not page.
    auto* bar = transcript_->verticalScrollBar();
    connect(bar, &QScrollBar::actionTriggered, this, [this, bar] {
        if (bar->sliderPosition() == bar->minimum() && session_ != nullptr && session_->hasOlder())
            session_->loadOlder();
    });
    connect(more_, &QToolButton::clicked, this, [this] {
        if (session_ == nullptr)
            return;
        QMenu* menu = actions_->menu(session_->sessionId(), this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(more_->mapToGlobal(QPoint(0, more_->height())));
    });
    connect(actions_, &MachineConversationActions::failed, this, [this](const QString& error) {
        actionError_ = error;
        render();
    });
    connect(actions_, &MachineConversationActions::pendingChanged, this,
            &MachineWorkspaceView::render);
    connect(actions_, &MachineConversationActions::finished, this, [this] {
        actionError_ = actions_->lastError();
        render();
    });
    connect(
        actions_, &MachineConversationActions::forked, this,
        [this](const QString& machine, const QString& source, const QString& sessionId, qint64) {
            // A fork the machine confirms after the reader moved on is only listed; it
            // never pulls the view away from what they read now.
            if (machine != catalog_->machineId() || openSessionId() != source)
                return;
            pendingFork_ = sessionId;
            pendingForkMachine_ = machine;
            catalogChanged();
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
    // A pending fork is dropped when its machine goes away or is switched away from.
    if (!pendingFork_.isEmpty() &&
        (catalog_->backend() == nullptr || machine != pendingForkMachine_))
        pendingFork_.clear();
    if (!pendingFork_.isEmpty()) {
        const remote::MachineConversation fork = catalog_->conversation(pendingFork_);
        if (!fork.sessionId.isEmpty()) {
            pendingFork_.clear();
            bind(fork.sessionId, fork.generation, fork.running());
            return;
        }
    }
    if (session_ == nullptr || session_->machineId() != machine) {
        showSession(lastByMachine_.value(machine), true);
        return;
    }
    // The listing is the machine's word on whether a turn still runs, for example one that
    // ended while the link was down.
    const remote::MachineConversation row = catalog_->conversation(session_->sessionId());
    if (!row.sessionId.isEmpty())
        session_->setRunning(row.running());
    render();
}

void MachineWorkspaceView::openConversation(const QString& sessionId) {
    // The reader chose what to read: a fork still waiting to be listed does not take over.
    if (sessionId != pendingFork_)
        pendingFork_.clear();
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
    showSession(session, true);
    emit conversationOpened(machine, sessionId);
}

void MachineWorkspaceView::startNewConversation(const QString& repositoryId) {
    newConversationRepository_ = repositoryId;
    pendingFork_.clear();
    const QString machine = catalog_->machineId();
    if (auto previous = lastByMachine_.take(machine); previous != nullptr)
        previous->deleteLater();
    showSession(nullptr, false);
    composer_->setFocus();
}

void MachineWorkspaceView::showSession(MachineConversationSession* session, const bool reload) {
    if (session_ != nullptr && session_ != session) {
        disconnect(session_, nullptr, this, nullptr);
        // Deselected or its machine switched away: the route stops following it, and its
        // cached transcript stays for when that machine is shown again.
        session_->close();
    }
    const bool shown = session_ == session;
    if (!shown)
        actionError_.clear();
    session_ = session;
    if (session_ != nullptr && !shown && reload)
        session_->load();
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

    renderTranscript();
    approvals_->refresh();
    renderActions(newConversation);

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
    else if (!actions_->pendingStatus().isEmpty())
        reason = actions_->pendingStatus();
    else if (!actionError_.isEmpty())
        reason = actionError_;
    else
        reason = session_->lastError();
    composer_->setReadOnly(!canAct);
    composer_->setEnabled(canAct);
    composerReason_->setText(reason);
    send_->setEnabled(canAct);
    stop_->setVisible(session_ != nullptr && session_->running());
    stop_->setEnabled(canAct);
}

void MachineWorkspaceView::renderTranscript() {
    const QString sessionId = session_ != nullptr ? session_->sessionId() : QString{};
    const qint64 firstCursor = session_ != nullptr ? session_->transcript().firstCursor() : 0;
    // An older page keeps the reader where they were; anything else follows the newest row.
    const bool sameSession = sessionId == renderedSession_;
    const bool olderAdded = sameSession && renderedFirstCursor_ > 0 && firstCursor > 0 &&
                            firstCursor < renderedFirstCursor_;
    // Only a reader at the newest row follows new rows; one reading older rows stays there.
    QScrollBar* bar = transcript_->verticalScrollBar();
    const int position = bar->value();
    const bool following = !sameSession || bar->maximum() == 0 || position >= bar->maximum() - 2;
    transcript_->clear();
    if (session_ != nullptr)
        for (const auto& item : session_->transcript().items()) {
            auto* row = new QListWidgetItem(item.text, transcript_);
            row->setData(Qt::UserRole, static_cast<int>(item.kind));
        }
    const int added = transcript_->count() - renderedRows_;
    if (olderAdded && added > 0 && added < transcript_->count()) {
        transcript_->scrollToItem(transcript_->item(added), QAbstractItemView::PositionAtTop);
    } else if (following) {
        transcript_->scrollToBottom();
    } else {
        transcript_->doItemsLayout();
        bar->setValue(position);
    }
    renderedSession_ = sessionId;
    renderedFirstCursor_ = firstCursor;
    renderedRows_ = transcript_->count();
    transcriptNote_->setVisible(session_ != nullptr);
    older_->setVisible(session_ != nullptr && session_->hasOlder());
    older_->setEnabled(session_ != nullptr && !session_->loadingOlder());
    older_->setText(session_ != nullptr && session_->loadingOlder() ? tr("Loading…")
                                                                    : tr("Show earlier messages"));
}

void MachineWorkspaceView::renderActions(const bool newConversation) {
    more_->setVisible(!newConversation);
    // Attachments are offered on every route and say why they cannot be used yet.
    QString reason;
    attach_->setEnabled(
        actions_->available(MachineConversationActions::Feature::Attachments, &reason));
    attach_->setToolTip(reason);
}

void MachineWorkspaceView::renderWorktrees() {
    const QString current = worktrees_->currentData().toString();
    const QSignalBlocker blocker(worktrees_);
    worktrees_->clear();
    auto* model = qobject_cast<QStandardItemModel*>(worktrees_->model());
    // A repository the machine does not list (a conversation's own worktree) offers them all.
    bool known = false;
    for (const auto& repository : catalog_->repositories())
        known = known || repository.id == newConversationRepository_;
    for (const auto& repository : catalog_->repositories()) {
        if (known && repository.id != newConversationRepository_)
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
