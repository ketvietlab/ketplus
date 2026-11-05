#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "runtime/ProviderHandoff.h"
#include "runtime/SessionHost.h"

#include <QJsonDocument>
#include <QPointer>

namespace ketplus::daemon {
namespace {
QJsonArray threadsOf(const StoredSession& session) {
    return provider_handoff::threadReferences(
        session.adapter, session.threadId,
        QJsonDocument::fromJson(session.providerThreads.toUtf8()).object());
}
bool referenced(const QList<StoredSession>& sessions, const QString& except,
                const QString& provider, const QString& threadId) {
    for (const auto& session : sessions)
        if (session.id != except && threadsOf(session).contains(QJsonObject{
                                        {"provider", provider}, {"thread_id", threadId}}))
            return true;
    return false;
}
} // namespace

QJsonArray SessionHost::providerCleanupTargets(const StoredSession& session) const {
    QJsonArray targets;
    const auto retained = journal_->sessions();
    for (const auto& value : threadsOf(session)) {
        const auto target = value.toObject();
        if (!referenced(retained, session.id, target.value("provider").toString(),
                        target.value("thread_id").toString()))
            targets.append(target);
    }
    return targets;
}

void SessionHost::processDeletionWork() {
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
            if (referenced(journal_->sessions(), {}, provider, thread)) {
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
