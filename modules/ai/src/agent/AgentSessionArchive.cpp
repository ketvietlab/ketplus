#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"
#include <QFileInfo>
#include <QJsonObject>
#include <algorithm>

namespace ketplus {

bool AgentSessionManager::archiveConversation(const QString& worktreePath,
                                              const QString& conversationId) {
    const SessionPtr current = ensureSession(worktreePath);
    if (!current || conversationId.isEmpty()) {
        return false;
    }
    auto& values = conversationsByPath_[current->path];
    const auto found =
        std::find_if(values.begin(), values.end(), [&conversationId](const SessionPtr& value) {
            return value->id == conversationId;
        });
    if (found == values.end() || (*found)->archived) {
        return false;
    }
    (*found)->archived = true;
    (*found)->archiveUnsent = true;
    syncArchived(*found);
    selectAwayFromArchived(*found);
    persistWorkspace(current->path);
    emit conversationsChanged(current->path, conversations(current->path));
    emit activityChanged();
    return true;
}

void AgentSessionManager::archiveConversationsForWorktree(const QString& worktreePath) {
    const QString path = normalizedPath(worktreePath);
    (void)ensureSession(path);
    bool changed = false;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (!session->archived) {
            session->archived = true;
            session->archiveUnsent = true;
            syncArchived(session);
            changed = true;
        }
    }
    if (changed) {
        persistWorkspace(path);
        emit conversationsChanged(path, conversations(path));
        emit activityChanged();
    }
}

bool AgentSessionManager::restoreConversation(const QString& worktreePath,
                                              const QString& conversationId) {
    if (!QFileInfo(worktreePath).isDir())
        return false;
    const QString path = normalizedPath(worktreePath);
    (void)ensureSession(path);
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id != conversationId || !session->archived) {
            continue;
        }
        const auto finish = [this](const SessionPtr& restored, const bool unsent) {
            restored->archived = false;
            restored->archiveUnsent = unsent;
            persistWorkspace(restored->path);
            emit conversationsChanged(restored->path, conversations(restored->path));
            emit activityChanged();
        };
        // The daemon may delete an archived conversation, so a restore it can record finishes
        // only once it has; until then the conversation stays archived.
        if (session->threadId.isEmpty() || !server_->hasArchiveSupport() ||
            !server_->hasDaemonSession(session->threadId)) {
            finish(session, !session->threadId.isEmpty());
            return true;
        }
        server_->setDaemonSessionArchived(
            session->threadId, false,
            [this, finish, weak = std::weak_ptr<Session>(session)](const QJsonObject&,
                                                                   const QString& error) {
                const SessionPtr current = weak.lock();
                if (!current)
                    return;
                if (!error.isEmpty()) {
                    emit errorOccurred(
                        QStringLiteral("The conversation was not restored: %1").arg(error));
                    return;
                }
                if (current->archived)
                    finish(current, false);
            });
        return true;
    }
    return false;
}

// A conversation that becomes archived while selected gives way to the newest visible one.
void AgentSessionManager::selectAwayFromArchived(const SessionPtr& session) {
    const QString path = session->path;
    if (sessions_.value(path) != session || session->running)
        return;
    auto& values = conversationsByPath_[path];
    const auto replacement = std::find_if(values.rbegin(), values.rend(),
                                          [](const SessionPtr& value) { return !value->archived; });
    if (replacement == values.rend()) {
        auto fresh = std::make_shared<Session>();
        fresh->path = path;
        values.append(fresh);
        sessions_.insert(path, fresh);
    } else {
        sessions_.insert(path, *replacement);
    }
    const SessionPtr selected = sessions_.value(path);
    emit messagesReset(path, messages(path));
    emit sessionStateChanged(path, selected->title, selected->status, selected->running);
    emitConfiguration(selected);
    emitHistoryState(selected);
}

// Applies the daemon catalog's archive state (ADR 0100). A change made here that the daemon
// has not confirmed is sent again; otherwise the daemon's state wins, so archiving or
// restoring from another client is not undone by this desktop's older copy.
void AgentSessionManager::applyDaemonArchiveState(const SessionPtr& session,
                                                  const bool archivedByDaemon) {
    if (!server_->hasArchiveSupport())
        return;
    if (session->archiveUnsent) {
        if (session->archived != archivedByDaemon)
            syncArchived(session);
        else if (!session->archiveSyncPending)
            session->archiveUnsent = false;
        return;
    }
    if (session->archived == archivedByDaemon)
        return;
    session->archived = archivedByDaemon;
    if (archivedByDaemon)
        selectAwayFromArchived(session);
}

// Sends an archive made here to the daemon so archived history can expire. Until the daemon
// confirms it, the change stays unsent and the next catalog refresh sends it again.
void AgentSessionManager::syncArchived(const SessionPtr& session) {
    if (!session || session->threadId.isEmpty() || !server_->hasArchiveSupport() ||
        session->archiveSyncPending)
        return;
    session->archiveSyncPending = true;
    const bool archived = session->archived;
    server_->setDaemonSessionArchived(
        session->threadId, archived,
        [this, archived, weak = std::weak_ptr<Session>(session)](const QJsonObject&,
                                                                 const QString& error) {
            const SessionPtr current = weak.lock();
            if (!current)
                return;
            current->archiveSyncPending = false;
            if (!error.isEmpty()) {
                qWarning("agent: archive state was not sent to the daemon: %s", qPrintable(error));
                return;
            }
            if (current->archived != archived) {
                syncArchived(current);
                return;
            }
            current->archiveUnsent = false;
            persistWorkspace(current->path);
        });
}

} // namespace ketplus
