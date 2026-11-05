#include "runtime/SessionHost.h"

#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "runtime/CompactWindowPolicy.h"

#include <QPointer>
#include <QUuid>

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumSessionIdLength = 128;

QJsonObject forkError(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

} // namespace

void SessionHost::forkSession(const QJsonObject& request,
                              std::function<void(const QJsonObject&)> callback) {
    const QString messageId = request.value(QStringLiteral("message_id")).toString();
    const QJsonObject cached = journal_->commandResult(messageId);
    if (!cached.isEmpty()) {
        callback(cached);
        return;
    }
    const auto finish = [this, messageId, callback](const QJsonObject& result) {
        journal_->storeCommandResult(messageId, result);
        callback(result);
    };

    Session* source = nullptr;
    const QJsonObject failure = checkedSession(request, &source);
    if (!failure.isEmpty()) {
        finish(failure);
        return;
    }
    const QString adapter = source->stored.adapter;
    if (!providerSupports(adapter, compat::capability::threadFork)) {
        finish(forkError(QStringLiteral("unsupported_provider_operation"),
                         QStringLiteral("This provider cannot fork a conversation.")));
        return;
    }
    if (source->stored.threadId.isEmpty()) {
        finish(forkError(QStringLiteral("nothing_to_fork"),
                         QStringLiteral("The conversation has no provider history to fork.")));
        return;
    }
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    QString forkId = payload.value(QStringLiteral("fork_session_id")).toString().trimmed();
    if (forkId.isEmpty())
        forkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (forkId.size() > maximumSessionIdLength || sessions_.contains(forkId) ||
        !journal_->sessionTombstone(forkId, 1).sessionId.isEmpty()) {
        finish(forkError(QStringLiteral("invalid_session_id"),
                         QStringLiteral("fork_session_id must name a new session.")));
        return;
    }

    const QString sourceId = source->stored.id;
    const qint64 sourceGeneration = source->stored.generation;
    const QJsonObject params{{QStringLiteral("threadId"), source->stored.threadId},
                             {QStringLiteral("cwd"), source->stored.worktreePath}};
    const QPointer<SessionHost> self(this);
    auto forked = [self, finish, sourceId, sourceGeneration, forkId](const QJsonObject& result,
                                                                     const QString& providerError) {
        if (self.isNull())
            return;
        if (!providerError.isEmpty()) {
            finish(forkError(QStringLiteral("provider_error"), providerError));
            return;
        }
        const QString threadId = result.value(QStringLiteral("thread"))
                                     .toObject()
                                     .value(QStringLiteral("id"))
                                     .toString();
        if (threadId.isEmpty()) {
            finish(forkError(QStringLiteral("provider_error"),
                             QStringLiteral("The provider returned no forked thread id.")));
            return;
        }
        finish(self->storeFork(sourceId, sourceGeneration, forkId, threadId));
    };
    if (adapter == QStringLiteral("claude")) {
        ProviderAdapter* const claude = provider(adapter);
        if (claude == nullptr) {
            finish(
                forkError(QStringLiteral("provider_unavailable"), missingProviderMessage(adapter)));
            return;
        }
        claude->call(QStringLiteral("thread/fork"), params, std::move(forked));
        return;
    }
    QJsonObject codexParams = params;
    QJsonObject config = context_harness::codexCompactConfig(source->stored.compactWindow);
    // The fork is a new conversation: its agent tools carry its own token (ADR 0027).
    if (const auto launch = agentLaunch(forkId, 1)) {
        const QJsonObject agentConfig = agent_tools::codexConfig(*launch);
        for (auto entry = agentConfig.constBegin(); entry != agentConfig.constEnd(); ++entry)
            config.insert(entry.key(), entry.value());
    }
    if (!config.isEmpty())
        codexParams.insert(QStringLiteral("config"), config);
    codexParams.insert(QStringLiteral("ephemeral"), false);
    // History is copied from our journal; returning all provider turns can exceed
    // the shared transport limit and interrupt unrelated active conversations.
    codexParams.insert(QStringLiteral("excludeTurns"), true);
    adapter_->call(QStringLiteral("thread/fork"), codexParams, std::move(forked));
}

QJsonObject SessionHost::storeFork(const QString& sourceId, const qint64 sourceGeneration,
                                   const QString& forkId, const QString& threadId) {
    const auto source = sessions_.constFind(sourceId);
    if (source == sessions_.cend() || source->stored.generation != sourceGeneration)
        return forkError(QStringLiteral("session_not_found"),
                         QStringLiteral("The conversation was deleted while it was forked."));
    Session fork;
    fork.stored = source->stored;
    fork.stored.id = forkId;
    fork.stored.generation = 1;
    fork.stored.threadId = threadId;
    fork.stored.turnId.clear();
    fork.stored.state = QStringLiteral("ready");
    // Other providers' threads stay with the source; switching the fork to one of them starts a
    // new thread from the handoff transcript instead of sharing the source's.
    fork.stored.providerThreads = QStringLiteral("{}");
    fork.stored.title = source->stored.title.isEmpty()
                            ? QStringLiteral("Fork")
                            : source->stored.title + QStringLiteral(" (fork)");
    QString journalError;
    if (!journal_->forkSession(sourceId, sourceGeneration, fork.stored, &journalError))
        return forkError(QStringLiteral("journal_error"), journalError);
    mediaStore_.copySessionReferences(sourceId, forkId);
    const qint64 throughCursor = fork.stored.lastCursor;
    const bool copiedActiveTurn = source->startingTurn ||
                                  source->stored.state == QStringLiteral("starting") ||
                                  source->stored.state == QStringLiteral("running") ||
                                  source->stored.state == QStringLiteral("waiting_for_input") ||
                                  source->stored.state == QStringLiteral("stopping");
    auto it = sessions_.insert(forkId, fork);
    sessionsByThread_.insert(threadId, forkId);
    QString numberError;
    if (journal_->assignConversationNumber(forkId, it->stored.generation, &numberError).isEmpty())
        qWarning("Unable to number conversation %s: %s", qUtf8Printable(forkId),
                 qUtf8Printable(numberError));
    (void)appendEvent(*it, QStringLiteral("session.forked"),
                      {{QStringLiteral("from_session_id"), sourceId},
                       {QStringLiteral("from_session_generation"), sourceGeneration},
                       {QStringLiteral("through_cursor"), throughCursor}});
    // Only the copy is settled: inherited running rows and pending questions must not
    // make clients wait on a provider turn that belongs to the source conversation.
    if (copiedActiveTurn)
        (void)appendEvent(*it, QStringLiteral("turn.completed"),
                          {{QStringLiteral("status"), QStringLiteral("interrupted")},
                           {QStringLiteral("reason"), QStringLiteral("fork_snapshot")}});
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("session_id"), forkId},
            {QStringLiteral("session_generation"), it->stored.generation},
            {QStringLiteral("worktree_path"), it->stored.worktreePath},
            {QStringLiteral("adapter"), it->stored.adapter},
            {QStringLiteral("title"), it->stored.title},
            {QStringLiteral("state"), it->stored.state},
            {QStringLiteral("current_turn_sequence"), it->stored.currentTurnSequence},
            {QStringLiteral("last_event_cursor"), it->stored.lastCursor},
            {QStringLiteral("last_seen_cursor"), it->stored.lastSeenCursor},
            {QStringLiteral("last_turn_started_at"), it->stored.lastTurnStartedAt},
            {QStringLiteral("number"), journal_->conversationNumber(forkId, it->stored.generation)},
            {QStringLiteral("forked_from_session_id"), sourceId}};
}

} // namespace ketplus::daemon
