#include "agent/CodexAppServer.h"

#include "DaemonClient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace ketplus {

void CodexAppServer::queueDaemonDiscovery(const QJsonObject& envelope) {
    const auto id = envelope.value("session_id").toString();
    const auto generation = envelope.value("session_generation").toInteger();
    const auto cursor = envelope.value("cursor").toInteger();
    if (id.isEmpty() || generation <= 0 || cursor <= 0)
        return;
    auto& observed = daemonDiscoveryCursors_[id];
    observed = std::max(observed, qMakePair(generation, cursor));
    if (daemonDiscoveryScheduled_)
        return;
    daemonDiscoveryScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        daemonDiscoveryScheduled_ = false;
        discoverDaemonSessions();
    });
}

void CodexAppServer::discoverDaemonSessions() {
    if (!daemon_ || !daemon_->isReady() || daemonSessionListInFlight_ || daemonDiscoveryInFlight_ ||
        daemonDiscoveryCursors_.isEmpty())
        return;
    daemonDiscoveryInFlight_ = true;
    const auto observed = std::exchange(daemonDiscoveryCursors_, {});
    const auto epoch = daemonDiscoveryEpoch_;
    daemon_->call(
        "session.list", {}, {}, -1,
        [this, observed, epoch](const QJsonObject& result, const QString& error) {
            if (epoch != daemonDiscoveryEpoch_)
                return;
            daemonDiscoveryInFlight_ = false;
            if (error.isEmpty()) {
                // Incremental discovery must not reset known sessions' windows,
                // replay cursors, active turns, or the user's selected conversation.
                for (const auto& value : result.value("sessions").toArray()) {
                    const auto stored = value.toObject();
                    const auto id = stored.value("session_id").toString();
                    const auto path = stored.value("worktree_path").toString();
                    if (id.isEmpty() || path.isEmpty() || daemonSessions_.contains(id))
                        continue;
                    DaemonSession session;
                    const QFileInfo info(path);
                    session.cwd = QDir::cleanPath(info.canonicalFilePath().isEmpty()
                                                      ? info.absoluteFilePath()
                                                      : info.canonicalFilePath());
                    session.generation = stored.value("session_generation").toInteger(1);
                    session.contextRevision = stored.value("context_revision").toInteger(1);
                    const auto effective = stored.value("effective").toObject();
                    session.options.insert("model", effective.value("model"));
                    session.options.insert("effort", effective.value("effort"));
                    session.cursor = daemonHasTranscriptWindow_
                                         ? stored.value("last_event_cursor").toInteger()
                                         : 0;
                    daemonSessions_.insert(id, session);
                    if (!daemonHasTranscriptWindow_)
                        restoringDaemonSessions_.insert(id);
                    emit daemonSessionDiscovered(stored);
                    emit notificationReceived(
                        "thread/backgroundTasks/updated",
                        {{"threadId", id}, {"tasks", stored.value("background_tasks")}});
                    if (!daemonHasTranscriptWindow_) {
                        daemonRestoreQueue_.append(id);
                        emit daemonSessionRestoreStarted(id);
                    }
                }
                // Events may arrive after the catalog snapshot but before its reply.
                // Replay that gap from the journal, without retaining large payloads.
                auto pending = std::exchange(daemonDiscoveryCursors_, {});
                for (auto it = observed.cbegin(); it != observed.cend(); ++it)
                    pending[it.key()] = std::max(pending.value(it.key()), it.value());
                for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
                    const auto session = daemonSessions_.constFind(it.key());
                    if (session != daemonSessions_.cend()) {
                        if (session->generation == it->first && session->cursor < it->second)
                            attachDaemonSession(it.key());
                    } else if (!observed.contains(it.key())) {
                        // A different session was created while this read was in flight.
                        daemonDiscoveryCursors_.insert(it.key(), it.value());
                    }
                }
                if (!daemonHasTranscriptWindow_) {
                    reorderDaemonRestoreQueue();
                    restoreNextDaemonSession();
                }
            }
            if (!daemonDiscoveryCursors_.isEmpty())
                QTimer::singleShot(0, this, &CodexAppServer::discoverDaemonSessions);
        });
}

} // namespace ketplus
