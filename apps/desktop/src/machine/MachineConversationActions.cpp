#include "machine/MachineConversationActions.h"

#include "MachineRows.h"
#include "machine/MachineCatalog.h"
#include "machine/MachineComposition.h"
#include "machine/MachineWorkspaceView.h"

#include <QAction>
#include <QCursor>
#include <QInputDialog>
#include <QMenu>
#include <QPointer>
#include <QUuid>

#include <memory>
#include <utility>

namespace ketplus {

MachineConversationActions::MachineConversationActions(MachineCatalog* catalog, QObject* parent)
    : QObject(parent), catalog_(catalog) {}

bool MachineConversationActions::available(const Feature feature, QString* reason) const {
    auto* backend = catalog_->backend();
    const bool supported = backend != nullptr && backend->supports(feature);
    if (reason != nullptr)
        *reason = supported ? QString{}
                  : backend ? backend->unsupportedReason(feature)
                            : tr("This machine is no longer connected.");
    return supported;
}

QString MachineConversationActions::pendingStatus(const QString& sessionId) const {
    const QString machine = catalog_->machineId();
    const Pending* latest = nullptr;
    for (const Pending& pending : pending_)
        if (pending.machineId == machine && pending.sessionId == sessionId &&
            (latest == nullptr || pending.order > latest->order))
            latest = &pending;
    return latest != nullptr ? latest->status : QString{};
}

bool MachineConversationActions::start(const Feature feature, const QString& sessionId,
                                       qint64* generation) {
    QString reason;
    if (!available(feature, &reason)) {
        lastError_ = reason;
        emit failed(reason);
        return false;
    }
    *generation = catalog_->conversation(sessionId).generation;
    return true;
}

remote::MachineBackend::Callback
MachineConversationActions::done(const QString& sessionId, const QString& sent,
                                 std::function<void(const QJsonObject&)> succeeded,
                                 std::function<void()> failedToo) {
    // A command the Relay queued finishes on commandCompleted(); the others answer at once.
    // Until it does, pendingStatus() says it was sent and waits for the machine.
    auto finish =
        [self = QPointer(this), succeeded = std::move(succeeded), failedToo = std::move(failedToo)](
            const QJsonObject& result, const QString& error, const QString& commandId = {}) {
            if (self == nullptr)
                return;
            if (!commandId.isEmpty())
                self->pending_.remove(commandId);
            if (!error.isEmpty()) {
                self->lastError_ = error;
                if (failedToo)
                    failedToo();
                emit self->failed(error);
            } else {
                self->lastError_.clear();
                self->catalog_->refresh();
                if (succeeded)
                    succeeded(result);
            }
            emit self->finished();
        };
    return [self = QPointer(this), backend = QPointer(catalog_->backend()), finish, sessionId,
            sent](const QJsonObject& result, const QString& error) mutable {
        const QString commandId = remote::queuedCommandId(result);
        if (!error.isEmpty() || commandId.isEmpty() || backend == nullptr || self == nullptr) {
            finish(result, error);
            return;
        }
        self->pending_.insert(commandId,
                              {backend->machineId(), sessionId, sent, ++self->nextOrder_});
        emit self->pendingChanged();
        // Both connections end with the command: completed, or its machine gone.
        auto connections =
            std::make_shared<std::pair<QMetaObject::Connection, QMetaObject::Connection>>();
        connections->first = connect(
            backend, &remote::MachineBackend::commandCompleted, self,
            [commandId, connections, finish](const QString& id, const QJsonObject& completed,
                                             const QString& failure) mutable {
                if (id != commandId)
                    return;
                disconnect(connections->first);
                disconnect(connections->second);
                finish(completed, failure, commandId);
            });
        // A machine that goes away answers nothing more: its command is not left waiting.
        connections->second =
            connect(backend, &QObject::destroyed, self, [self, commandId, connections] {
                disconnect(connections->first);
                disconnect(connections->second);
                if (self != nullptr && self->pending_.remove(commandId) > 0)
                    emit self->pendingChanged();
            });
    };
}

void MachineConversationActions::rename(const QString& sessionId, const QString& title) {
    const QString trimmed = title.trimmed();
    qint64 generation = 0;
    if (trimmed.isEmpty() || !start(Feature::Rename, sessionId, &generation))
        return;
    catalog_->backend()->renameConversation(
        sessionId, generation, trimmed,
        done(sessionId, tr("Rename sent. Waiting for the machine…")));
}

void MachineConversationActions::fork(const QString& sessionId) {
    qint64 generation = 0;
    if (!start(Feature::Fork, sessionId, &generation))
        return;
    // The client names the fork, so a retried request cannot fork twice; the name is also the
    // request's id in forkRequested(), forked() and forkFailed().
    const QString forkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString machine = catalog_->machineId();
    emit forkRequested(machine, forkId, sessionId);
    catalog_->backend()->forkConversation(
        sessionId, generation, forkId,
        done(
            sessionId, tr("Fork sent. Waiting for the machine…"),
            [this, machine, forkId](const QJsonObject& result) {
                const QString id = result.value(QLatin1String("session_id")).toString(forkId);
                emit forked(machine, forkId, id,
                            result.value(QLatin1String("session_generation")).toInteger(1));
            },
            [this, forkId] { emit forkFailed(forkId); }));
}

void MachineConversationActions::archive(const QString& sessionId) {
    qint64 generation = 0;
    if (!start(Feature::Archive, sessionId, &generation))
        return;
    catalog_->backend()->setConversationArchived(
        sessionId, generation, true, done(sessionId, tr("Archive sent. Waiting for the machine…")));
}

QMenu* MachineConversationActions::menu(const QString& sessionId, QWidget* parent) {
    auto* menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("machineConversationMenu"));
    menu->setToolTipsVisible(true);
    const auto add = [&](const QString& text, const Feature feature, const QString& name,
                         std::function<void()> run) {
        QString reason;
        QAction* action = menu->addAction(text);
        action->setObjectName(name);
        action->setEnabled(available(feature, &reason));
        action->setToolTip(reason);
        connect(action, &QAction::triggered, this, std::move(run));
    };
    QPointer<QWidget> dialogParent = parent;
    add(tr("Rename…"), Feature::Rename, QStringLiteral("machineRenameAction"),
        [this, sessionId, dialogParent] { promptRename(sessionId, dialogParent); });
    add(tr("Fork"), Feature::Fork, QStringLiteral("machineForkAction"),
        [this, sessionId] { fork(sessionId); });
    add(tr("Archive"), Feature::Archive, QStringLiteral("machineArchiveAction"),
        [this, sessionId] { archive(sessionId); });
    return menu;
}

void MachineConversationActions::promptRename(const QString& sessionId, QWidget* parent) {
    QString reason;
    if (!available(Feature::Rename, &reason)) {
        lastError_ = reason;
        emit failed(reason);
        return;
    }
    bool accepted = false;
    const QString title =
        QInputDialog::getText(parent, tr("Rename Conversation"), tr("Title"), QLineEdit::Normal,
                              catalog_->conversation(sessionId).title, &accepted);
    if (accepted)
        rename(sessionId, title);
}

void showMachineConversationMenu(MachineComposition* composition, const QString& sessionId,
                                 QWidget* parent) {
    MachineWorkspaceView* view = composition != nullptr ? composition->existingView() : nullptr;
    if (view == nullptr)
        return; // remote rows are listed only while the remote workspace exists
    QMenu* menu = view->actions()->menu(sessionId, parent);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(QCursor::pos());
}

void renameMachineConversation(MachineComposition* composition, const QString& sessionId,
                               QWidget* parent) {
    MachineWorkspaceView* view = composition != nullptr ? composition->existingView() : nullptr;
    if (view == nullptr)
        return;
    // Leave the double-click event before running the modal dialog.
    QMetaObject::invokeMethod(
        view,
        [actions = QPointer(view->actions()), sessionId, parent = QPointer(parent)] {
            if (actions != nullptr)
                actions->promptRename(sessionId, parent);
        },
        Qt::QueuedConnection);
}

} // namespace ketplus
