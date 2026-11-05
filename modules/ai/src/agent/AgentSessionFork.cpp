#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"
#include "agent/ProviderRuntimeStatus.h"

#include <QDateTime>
#include <QJsonObject>

namespace ketplus {

bool AgentSessionManager::isForkable(const SessionPtr& session) const {
    // The daemon snapshots the provider thread and journal without stopping the source turn.
    if (!server_->usesDaemon() || session->threadId.isEmpty())
        return false;
    QString provider = session->options.value(QStringLiteral("provider")).toString();
    if (provider.isEmpty())
        provider = QStringLiteral("codex");
    return providerStatus_->supports(provider, QStringLiteral("threadFork"));
}

void AgentSessionManager::forkConversation(const QString& worktreePath,
                                           const QString& conversationId) {
    const QString path = normalizedPath(worktreePath);
    SessionPtr source;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id == conversationId) {
            source = session;
            break;
        }
    }
    if (!source || !isForkable(source)) {
        emit conversationForkFinished(path, conversationId, {},
                                      QStringLiteral("This conversation cannot be forked."));
        return;
    }
    server_->call(
        QStringLiteral("thread/fork"), {{QStringLiteral("threadId"), source->threadId}},
        [this, path, source](const QJsonObject& result, const QString& error) {
            // The daemon has already announced the fork as a discovered session.
            const QString forkThread = result.value(QStringLiteral("thread"))
                                           .toObject()
                                           .value(QStringLiteral("id"))
                                           .toString();
            const SessionPtr fork = error.isEmpty() ? sessionForThread(forkThread) : nullptr;
            if (!fork) {
                emit conversationForkFinished(
                    path, source->id, {},
                    error.isEmpty() ? QStringLiteral("The forked conversation did not appear.")
                                    : error);
                return;
            }
            fork->model = source->model;
            fork->effort = source->effort;
            fork->permission = source->permission;
            const QVariant provider = fork->options.value(QStringLiteral("provider"));
            fork->options = source->options;
            if (provider.isValid())
                fork->options.insert(QStringLiteral("provider"), provider);
            fork->listed = true;
            // Lists the fork first, as a conversation that has just been started would be. A
            // daemon stamped that time on the fork already, for phones too.
            if (!server_->usesDaemon())
                fork->lastTurnStartedAt = QDateTime::currentMSecsSinceEpoch();
            persistWorkspace(fork->path);
            selectConversation(fork->path, fork->id);
            emit conversationsChanged(fork->path, conversations(fork->path));
            emit activityChanged();
            emit conversationForkFinished(path, source->id, fork->id, {});
        });
}

} // namespace ketplus
