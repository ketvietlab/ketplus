#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "runtime/ProviderHandoff.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>
#include <QPointer>

namespace ketplus::daemon {
namespace {
std::optional<QJsonArray> threadsOf(const StoredSession& session) {
    const auto document = QJsonDocument::fromJson(session.providerThreads.toUtf8());
    if (!document.isObject())
        return std::nullopt;
    const auto threads = document.object();
    for (auto provider = threads.begin(); provider != threads.end(); ++provider) {
        if (provider.key().isEmpty() || !provider.value().isObject())
            return std::nullopt;
        const auto entry = provider.value().toObject();
        // Unknown fields may carry ownership in a newer format. Preserve them
        // for recovery instead of proving absence from a partial interpretation.
        for (auto field = entry.begin(); field != entry.end(); ++field)
            if (field.key() != "thread_id" && field.key() != "retired_thread_ids")
                return std::nullopt;
        if (entry.contains("thread_id") && !entry.value("thread_id").isString())
            return std::nullopt;
        if (entry.contains("retired_thread_ids")) {
            if (!entry.value("retired_thread_ids").isArray())
                return std::nullopt;
            for (const auto& id : entry.value("retired_thread_ids").toArray())
                if (!id.isString())
                    return std::nullopt;
        }
    }
    return provider_handoff::threadReferences(session.adapter, session.threadId, threads);
}
using Ownership = QList<QPair<QString, QJsonArray>>;
std::optional<Ownership> readOwnership(const LocalJournal& journal) {
    const auto sessions = journal.readSessions();
    if (!sessions)
        return std::nullopt;
    Ownership result;
    for (const auto& session : *sessions) {
        const auto threads = threadsOf(session);
        if (!threads)
            return std::nullopt;
        result.append({session.id, *threads});
    }
    return result;
}
bool referenced(const Ownership& ownership, const QString& except, const QString& provider,
                const QString& threadId) {
    const QJsonObject target{{"provider", provider}, {"thread_id", threadId}};
    for (const auto& session : ownership)
        if (session.first != except && session.second.contains(target))
            return true;
    return false;
}
} // namespace

std::optional<QJsonArray> SessionHost::providerCleanupTargets(const StoredSession& session) const {
    QJsonArray targets;
    const auto retained = readOwnership(*journal_);
    const auto owned = threadsOf(session);
    if (!retained || !owned)
        return std::nullopt;
    for (const auto& value : *owned) {
        const auto target = value.toObject();
        if (!referenced(*retained, session.id, target.value("provider").toString(),
                        target.value("thread_id").toString()))
            targets.append(target);
    }
    return targets;
}

void SessionHost::scheduleHistoryPurge(const int delayMs) {
    if (historyPurgeScheduled_)
        return;
    historyPurgeScheduled_ = true;
    QTimer::singleShot(delayMs, this, [this] {
        historyPurgeScheduled_ = false;
        purgeDeletedHistory();
    });
}

void SessionHost::purgeDeletedHistory() {
    // One pass per event-loop turn keeps IPC responsive while a large history drains.
    switch (journal_->purgeDeletedSessionHistory()) {
    case LocalJournal::HistoryPurge::Progress:
        scheduleHistoryPurge(0);
        return;
    case LocalJournal::HistoryPurge::Deferred:
        scheduleHistoryPurge(250);
        return;
    case LocalJournal::HistoryPurge::Failed:
        // The marker stays durable; the periodic deletion work retries.
        qWarning("Deleted session history cleanup deferred: %s",
                 qUtf8Printable(journal_->errorString()));
        return;
    case LocalJournal::HistoryPurge::Idle:
        return;
    }
}

void SessionHost::processDeletionWork() {
    if (journal_->hasPendingHistoryPurge())
        scheduleHistoryPurge(0);
    for (const auto& value : journal_->deletionWork(64, false, deletionWorkCursor_)) {
        const auto work = value.toObject();
        deletionWorkCursor_ = work.value("work_id").toInteger();
        const auto deletion = work.value("deletion").toObject();
        const auto id = deletion.value("sessionId").toString();
        const auto generation = deletion.value("sessionGeneration").toInteger();
        for (const auto& value : work.value("provider_targets").toArray()) {
            if (providerCleanupInFlight_.size() >= 4)
                return;
            const auto target = value.toObject();
            const auto provider = target.value("provider").toString();
            const auto thread = target.value("thread_id").toString();
            const auto key = provider + ':' + thread;
            if (providerCleanupInFlight_.contains(key))
                continue;
            // A retained conversation wins over a queued cleanup, including after restart.
            const auto retained = readOwnership(*journal_);
            if (!retained) {
                qWarning("Provider cleanup deferred: retained session ownership is unreadable or "
                         "unsupported.");
                return;
            }
            if (referenced(*retained, {}, provider, thread)) {
                journal_->completeProviderCleanup(id, generation, provider, thread);
                continue;
            }
            // Only Codex and Claude delete provider threads.
            ProviderAdapter* const adapter =
                provider == "codex" || provider == "claude" ? availableProvider(provider) : nullptr;
            if (adapter == nullptr)
                continue; // Keep unsupported/unavailable cleanup durable.
            providerCleanupInFlight_.insert(key);
            const QPointer<SessionHost> guard(this);
            auto completed = [guard, id, generation, provider, thread, key](const QJsonObject&,
                                                                            const QString& error) {
                if (!guard)
                    return;
                guard->providerCleanupInFlight_.remove(key);
                const bool missing =
                    error.contains(thread) && error.contains("not found", Qt::CaseInsensitive);
                if (error.isEmpty() || missing) {
                    guard->journal_->completeProviderCleanup(id, generation, provider, thread);
                    QTimer::singleShot(0, guard, &SessionHost::processDeletionWork);
                } else {
                    qWarning("Provider thread cleanup deferred (%s): %s", qUtf8Printable(provider),
                             qUtf8Printable(error));
                }
            };
            const QJsonObject args{{"threadId", thread}};
            adapter->call("thread/delete", args, std::move(completed));
        }
    }
}
} // namespace ketplus::daemon
