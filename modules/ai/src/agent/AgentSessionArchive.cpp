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
    syncArchived(*found);
    if (sessions_.value(current->path) == *found && !(*found)->running) {
        const auto replacement =
            std::find_if(values.rbegin(), values.rend(),
                         [](const SessionPtr& value) { return !value->archived; });
        if (replacement == values.rend()) {
            auto fresh = std::make_shared<Session>();
            fresh->path = current->path;
            values.append(fresh);
            sessions_.insert(current->path, fresh);
        } else {
            sessions_.insert(current->path, *replacement);
        }
        const SessionPtr selected = sessions_.value(current->path);
        emit messagesReset(current->path, messages(current->path));
        emit sessionStateChanged(current->path, selected->title, selected->status,
                                 selected->running);
        emitConfiguration(selected);
        emitHistoryState(selected);
    }
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
        session->archived = false;
        syncArchived(session);
        persistWorkspace(path);
        emit conversationsChanged(path, conversations(path));
        emit activityChanged();
        return true;
    }
    return false;
}

// Tells the daemon when a conversation is archived or restored, so archived history can expire.
// This desktop owns the archive state; a mismatch found later in the catalog is sent again.
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
            if (current->archived != archived)
                syncArchived(current);
        });
}

} // namespace ketplus
