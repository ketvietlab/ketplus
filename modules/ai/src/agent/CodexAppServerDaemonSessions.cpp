#include "BrowserAgentInstructions.h"
#include "agent/CodexAppServer.h"

#include "agent/DaemonTranscriptProjection.h"
#include <algorithm>

#include "CodexBrowserTools.h"
#include "CodexLaunchReloader.h"
#include "CodexServerRequests.h"
#include "DaemonClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

// Daemon mode: calls, the session list and restoring attached conversations.

namespace ketplus {
namespace {

constexpr int legacyRestoreReplayBatchSize = 16;
constexpr int restoreReplayBatchSize = 128;

QString normalizedLocalPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

} // namespace

void CodexAppServer::setupDaemonTransport() {
    daemon_ = new remote::DaemonClient(this);
    daemon_->setActivityGroupsEnabled(true);
    executable_ = daemon_->isAvailable() ? QStringLiteral("ketplus_daemon") : QString{};
    connect(daemon_, &remote::DaemonClient::readyChanged, this, [this](const bool ready) {
        setReady(ready);
        if (ready) {
            refreshDaemonSessions();
        } else {
            daemonSessionListInFlight_ = false;
            ++daemonSessionListEpoch_;
            ++daemonDiscoveryEpoch_;
            daemonDiscoveryInFlight_ = false;
            daemonDiscoveryCursors_.clear();
            restoringDaemonSessions_.clear();
            daemonRestoreQueue_.clear();
            activeDaemonRestoreSession_.clear();
            setRestoring(false);
        }
    });
    connect(daemon_, &remote::DaemonClient::eventReceived, this,
            &CodexAppServer::consumeDaemonEvent);
    connect(daemon_, &remote::DaemonClient::disconnected, this, [](const QString&) {});
}

CodexAppServer::~CodexAppServer() { stop(); }

void CodexAppServer::prioritizeDaemonRestore(const QString& worktreePath,
                                             const QString& sessionId) {
    daemonRestorePriorityPath_ = normalizedLocalPath(worktreePath);
    daemonRestorePrioritySession_ = sessionId;
    reorderDaemonRestoreQueue();
    restoreNextDaemonSession();
}

bool CodexAppServer::isDaemonSessionRestoring(const QString& sessionId) const {
    return daemon_ != nullptr && !sessionId.isEmpty() &&
           (daemonSessionListInFlight_ || restoringDaemonSessions_.contains(sessionId));
}

void CodexAppServer::executionWorktreeCommand(const QString& id, const QString& type,
                                              const QJsonObject& payload,
                                              ResponseCallback callback) {
    const auto found = daemonSessions_.constFind(id);
    if (!daemon_ || found == daemonSessions_.cend()) {
        callback({}, "Conversation is not attached to the daemon.");
        return;
    }
    daemon_->call(
        type, payload, id, found->generation,
        [this, id, callback = std::move(callback)](const QJsonObject& result,
                                                   const QString& error) {
            auto it = daemonSessions_.find(id);
            if (error.isEmpty() && it != daemonSessions_.end() &&
                result.contains("context_revision")) {
                if (result.value("context_revision").toInteger() < it->contextRevision) {
                    callback({}, "The conversation context changed. Refresh the picker.");
                    return;
                }
                it->contextRevision = result.value("context_revision").toInteger();
                it->cwd = result.value("worktree_path").toString(it->cwd);
                emit executionContextChanged(
                    id, result.value("current_branch").toString(QFileInfo(it->cwd).fileName()),
                    it->cwd, it->contextRevision);
            }
            callback(result, error);
        });
}

void CodexAppServer::daemonCall(const QString& method, const QJsonObject& params,
                                ResponseCallback callback) {
    if (!isAvailable()) {
        callback({}, QStringLiteral("KetPlus Daemon executable was not found."));
        return;
    }
    if (method.startsWith(QStringLiteral("daemon/storage"))) {
        const bool retention = method.startsWith(QStringLiteral("daemon/storageRetention/"));
        const bool size = method.startsWith(QStringLiteral("daemon/storageSize/"));
        const QString operation = method.section('/', -1);
        const QString capability =
            (retention ? QStringLiteral("storage.retention.") : QStringLiteral("storage.size.")) +
            operation;
        if ((!retention && !size) || method.count('/') != 2 ||
            (operation != QStringLiteral("read") && operation != QStringLiteral("apply")) ||
            !daemon_->isReady() || !daemon_->hasCapability(capability)) {
            callback(
                {},
                QStringLiteral(
                    "Connect to an updated local KetPlus daemon, then reload storage settings."));
            return;
        }
        daemon_->call(capability, params, {}, -1, std::move(callback));
        return;
    }
    if (method == QStringLiteral("model/list")) {
        daemon_->call(QStringLiteral("provider.models"),
                      {{QStringLiteral("cursor"), params.value(QStringLiteral("cursor"))}}, {}, -1,
                      std::move(callback));
        return;
    }
    if (method == QStringLiteral("account/rateLimits/read")) {
        daemon_->call(QStringLiteral("provider.rate_limits"), {}, {}, -1, std::move(callback));
        return;
    }
    if (method.startsWith(QStringLiteral("provider/claudeCredentials/"))) {
        if (!daemon_->isReady() ||
            !daemon_->hasFeature(QStringLiteral("provider.claude.credentials/1"))) {
            callback(
                {},
                QStringLiteral(
                    "Connect to an updated local KetPlus daemon to configure Claude API keys."));
            return;
        }
        const QString operation = method.mid(QStringLiteral("provider/claudeCredentials/").size());
        daemon_->call(QStringLiteral("provider.claude.credentials.") + operation, params, {}, -1,
                      std::move(callback));
        return;
    }
    if (method == QStringLiteral("provider/runtimeStatus")) {
        // Daemons before protocol minor 1 do not report provider runtimes.
        if (daemon_->isReady() && !daemon_->hasFeature(QStringLiteral("provider.runtime_status"))) {
            callback({}, QStringLiteral("KetPlus Daemon does not support %1 yet.").arg(method));
            return;
        }
        daemon_->call(QStringLiteral("provider.runtime_status"), {}, {}, -1, std::move(callback));
        return;
    }
    if (method == QStringLiteral("thread/start")) {
        const QString sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        DaemonSession session;
        session.cwd = params.value(QStringLiteral("cwd")).toString();
        session.options = params;
        session.windowReady = true;
        daemonSessions_.insert(sessionId, session);
        daemonRestorePrioritySession_ = sessionId;
        callback({{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), sessionId},
                                                         {QStringLiteral("turns"), QJsonArray{}}}}},
                 {});
        return;
    }
    if (method == QStringLiteral("thread/resume")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto& session = daemonSessions_[sessionId];
        if (session.cwd.isEmpty())
            session.cwd = params.value(QStringLiteral("cwd")).toString();
        session.options = params;
        callback({{QStringLiteral("thread"), QJsonObject{{QStringLiteral("id"), sessionId},
                                                         {QStringLiteral("turns"), QJsonArray{}}}}},
                 {});
        return;
    }
    if (method == QStringLiteral("thread/turns/list")) {
        callback({{QStringLiteral("data"), QJsonArray{}}}, {});
        return;
    }
    if (method == QStringLiteral("thread/read")) {
        callback({{QStringLiteral("thread"),
                   QJsonObject{{QStringLiteral("id"), params.value(QStringLiteral("threadId"))},
                               {QStringLiteral("turns"), QJsonArray{}}}}},
                 {});
        return;
    }
    if (method == QStringLiteral("thread/name/set")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        daemon_->call(QStringLiteral("session.rename"),
                      {{QStringLiteral("title"), params.value(QStringLiteral("name"))}}, sessionId,
                      found->generation, std::move(callback));
        return;
    }
    if (method == QStringLiteral("thread/delete")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        const qint64 generation = found->generation;
        daemon_->call(QStringLiteral("session.delete"), {}, sessionId, generation,
                      [this, sessionId, callback = std::move(callback)](
                          const QJsonObject& result, const QString& error) mutable {
                          if (error.isEmpty())
                              daemonSessions_.remove(sessionId);
                          callback(result, error);
                      });
        return;
    }
    if (method == QStringLiteral("thread/fork")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        if (daemon_->isReady() && !daemon_->hasFeature(QStringLiteral("session.fork"))) {
            callback({}, QStringLiteral("KetPlus Daemon does not support %1 yet.").arg(method));
            return;
        }
        // The client names the fork so a retried request cannot fork twice.
        const QString forkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        daemon_->call(
            QStringLiteral("session.fork"), {{QStringLiteral("fork_session_id"), forkId}},
            sessionId, found->generation,
            [this, sessionId, callback = std::move(callback)](const QJsonObject& result,
                                                              const QString& error) {
                if (!error.isEmpty()) {
                    callback({}, error);
                    return;
                }
                const QString forkId = result.value(QStringLiteral("session_id")).toString();
                DaemonSession fork;
                if (const auto source = daemonSessions_.constFind(sessionId);
                    source != daemonSessions_.cend())
                    fork.options = source->options;
                fork.options.insert(QStringLiteral("provider"),
                                    result.value(QStringLiteral("adapter")));
                fork.cwd =
                    normalizedLocalPath(result.value(QStringLiteral("worktree_path")).toString());
                fork.generation = result.value(QStringLiteral("session_generation")).toInteger(1);
                // The copied transcript is read through the transcript window when it is shown.
                fork.cursor = result.value(QStringLiteral("last_event_cursor")).toInteger();
                fork.windowReady = !daemonHasTranscriptWindow_;
                daemonSessions_.insert(forkId, fork);
                emit daemonSessionDiscovered(result);
                callback({{QStringLiteral("thread"),
                           QJsonObject{{QStringLiteral("id"), forkId},
                                       {QStringLiteral("forkedFromId"), sessionId}}}},
                         {});
            });
        return;
    }
    if (method == QStringLiteral("turn/start")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end() || found->cwd.isEmpty()) {
            callback({}, QStringLiteral("The daemon session has no worktree."));
            return;
        }
        QString prompt;
        for (const QJsonValue& value : params.value(QStringLiteral("input")).toArray()) {
            const QJsonObject input = value.toObject();
            if (input.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                prompt = input.value(QStringLiteral("text")).toString();
                break;
            }
        }
        QJsonObject options;
        for (const QString& key : {QStringLiteral("model"), QStringLiteral("effort")}) {
            QString value = params.value(key).toString();
            if (value.isEmpty()) {
                value = found->options.value(key).toString();
            }
            if (!value.isEmpty()) {
                options.insert(key, value);
            }
        }
        const QString sandboxType = params.value(QStringLiteral("sandboxPolicy"))
                                        .toObject()
                                        .value(QStringLiteral("type"))
                                        .toString();
        options.insert(QStringLiteral("permission"),
                       sandboxType == QStringLiteral("readOnly") ? QStringLiteral("read-only")
                       : sandboxType == QStringLiteral("dangerFullAccess")
                           ? QStringLiteral("danger-full-access")
                           : QStringLiteral("workspace-write"));
        const QString provider = params.value(QStringLiteral("provider"))
                                     .toString(found->options.value(QStringLiteral("provider"))
                                                   .toString(QStringLiteral("codex")));
        if (!provider.isEmpty())
            options.insert(QStringLiteral("provider"), provider);
        found->turnStartInFlight = true;
        daemon_->call(
            QStringLiteral("session.start_turn"),
            {{QStringLiteral("worktree_path"), found->cwd},
             {QStringLiteral("expected_context_revision"), found->contextRevision},
             {QStringLiteral("prompt"), prompt},
             {QStringLiteral("display_text"), params.value(QStringLiteral("displayText"))},
             {QStringLiteral("client_message_id"), params.value(QStringLiteral("clientMessageId"))},
             {QStringLiteral("input"), params.value(QStringLiteral("input"))},
             {QStringLiteral("options"), options}},
            sessionId, found->generation,
            [this, sessionId, callback = std::move(callback)](const QJsonObject& result,
                                                              const QString& error) {
                auto found = daemonSessions_.find(sessionId);
                if (found != daemonSessions_.end()) {
                    found->turnStartInFlight = false;
                    if (!error.isEmpty())
                        found->interruptRequested = false;
                    else if (std::exchange(found->interruptRequested, false))
                        daemon_->call(QStringLiteral("session.interrupt"), {}, sessionId,
                                      found->generation, [](const QJsonObject&, const QString&) {});
                }
                if (!error.isEmpty()) {
                    callback({}, error);
                    return;
                }
                if (found == daemonSessions_.end()) {
                    callback({}, QStringLiteral("The daemon session disappeared."));
                    return;
                }
                found->generation =
                    result.value(QStringLiteral("session_generation")).toInteger(found->generation);
                found->cursor = qMax(found->cursor,
                                     result.value(QStringLiteral("last_event_cursor")).toInteger());
                if (const QString number = result.value(QStringLiteral("number")).toString();
                    !number.isEmpty())
                    emit notificationReceived(QStringLiteral("daemon/conversationNumber"),
                                              {{QStringLiteral("threadId"), sessionId},
                                               {QStringLiteral("number"), number}});
                // The daemon has only queued the turn. Report the real id (possibly empty) so a
                // Stop pressed now stays pending until turn.started instead of interrupting a
                // fabricated turn that the daemon rejects.
                callback(
                    {{QStringLiteral("turn"), QJsonObject{{QStringLiteral("id"), found->turnId}}}},
                    {});
            });
        return;
    }
    if (method == QStringLiteral("turn/steer")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        QString prompt;
        const QJsonArray input = params.value(QStringLiteral("input")).toArray();
        if (!input.isEmpty()) {
            prompt = input.first().toObject().value(QStringLiteral("text")).toString();
        }
        // The whole input goes along: the daemon hands its images to the provider.
        daemon_->call(
            QStringLiteral("session.steer"),
            {{QStringLiteral("prompt"), prompt},
             {QStringLiteral("display_text"), params.value(QStringLiteral("displayText"))},
             {QStringLiteral("input"), input},
             {"expected_context_revision", found->contextRevision}},
            sessionId, found->generation, std::move(callback));
        return;
    }
    if (method == QStringLiteral("turn/interrupt")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        if (found->turnStartInFlight) {
            found->interruptRequested = true;
            callback({{QStringLiteral("accepted"), true}}, {});
            return;
        }
        daemon_->call(QStringLiteral("session.interrupt"), {}, sessionId, found->generation,
                      std::move(callback));
        return;
    }
    if (method == QStringLiteral("task/stop") || method == QStringLiteral("task/output")) {
        const QString sessionId = params.value(QStringLiteral("threadId")).toString();
        auto found = daemonSessions_.find(sessionId);
        if (found == daemonSessions_.end()) {
            callback({}, QStringLiteral("Unknown daemon session."));
            return;
        }
        daemon_->call(method == QStringLiteral("task/stop") ? QStringLiteral("session.task.stop")
                                                            : QStringLiteral("session.task.output"),
                      {{QStringLiteral("task_id"), params.value(QStringLiteral("taskId"))}},
                      sessionId, found->generation, std::move(callback));
        return;
    }
    callback({}, QStringLiteral("KetPlus Daemon does not support %1 yet.").arg(method));
}

qint64 CodexAppServer::daemonSessionCursor(const QString& sessionId, qint64 generation) const {
    const auto found = daemonSessions_.constFind(sessionId);
    return found != daemonSessions_.cend() && found->generation == generation ? found->cursor : 0;
}

void CodexAppServer::markDaemonSessionSeen(const QString& sessionId, const qint64 cursor,
                                           ResponseCallback callback) {
    if (!daemon_ || sessionId.isEmpty()) {
        callback({}, QStringLiteral("Daemon session marking is unavailable."));
        return;
    }
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end()) {
        callback({}, QStringLiteral("Unknown daemon session."));
        return;
    }
    QJsonObject payload;
    if (cursor >= 0)
        payload.insert(QStringLiteral("cursor"), cursor);
    daemon_->call(QStringLiteral("session.mark_seen"), payload, sessionId, found->generation,
                  std::move(callback));
}

void CodexAppServer::applyDaemonTombstones(const QJsonArray& tombstones) {
    for (const auto& value : tombstones) {
        const auto deleted = value.toObject();
        const auto id = deleted.value("session_id").toString();
        const auto generation = deleted.value("session_generation").toInteger();
        const auto found = daemonSessions_.constFind(id);
        if (found != daemonSessions_.cend() && found->generation == generation)
            daemonSessions_.remove(id);
        emit daemonSessionDeleted(id, generation);
    }
}

void CodexAppServer::readDaemonSessionCatalogPage(const quint64 epoch, QJsonObject after,
                                                  ResponseCallback callback,
                                                  const bool liveCatalog) {
    if (epoch != daemonSessionListEpoch_ || !daemon_->isReady())
        return;
    QJsonObject payload{{"deleted_sessions_page_version", 1},
                        {"deleted_sessions_only", !liveCatalog}};
    if (!after.isEmpty())
        payload.insert("deleted_sessions_after", after);
    daemon_->call(
        "session.list", payload, {}, -1,
        [this, epoch, after, liveCatalog,
         callback = std::move(callback)](const QJsonObject& result, const QString& error) mutable {
            if (epoch != daemonSessionListEpoch_)
                return;
            if (!error.isEmpty()) {
                callback({}, error);
                return;
            }
            if (after.isEmpty() && !liveCatalog &&
                !result.contains("deleted_sessions_page_version")) {
                callback(result, {}); // Existing daemons return the complete legacy response.
                return;
            }
            const auto fail = [&] { callback({}, "Invalid daemon tombstone page."); };
            const auto rows = result.value("deleted_sessions").toArray();
            if (result.value("deleted_sessions_page_version").toInteger(-1) != 1 ||
                !result.value("deleted_sessions").isArray() || rows.size() > 128 ||
                QJsonDocument(rows).toJson(QJsonDocument::Compact).size() > 64 * 1024 ||
                !result.value("deleted_sessions_next").isObject()) {
                fail();
                return;
            }
            auto previous = after;
            for (const auto& value : rows) {
                const auto row = value.toObject();
                const auto id = row.value("session_id").toString().toUtf8();
                const auto generation = row.value("session_generation").toInteger(-1);
                const auto previousId = previous.value("session_id").toString().toUtf8();
                if (row.size() != 2 || id.isEmpty() || id.size() > 1024 || generation < 1 ||
                    (!previous.isEmpty() &&
                     (id < previousId ||
                      (id == previousId &&
                       generation <= previous.value("session_generation").toInteger())))) {
                    fail();
                    return;
                }
                previous = row;
            }
            const auto next = result.value("deleted_sessions_next").toObject();
            if (!next.isEmpty() && (rows.isEmpty() || next != previous)) {
                fail();
                return;
            }
            if (liveCatalog) {
                // Read live sessions after the walk, so an intervening session.deleted
                // event cannot be undone by replaying an older catalog snapshot.
                callback(result, {});
                return;
            }
            applyDaemonTombstones(rows);
            QTimer::singleShot(
                0, this, [this, epoch, next, callback = std::move(callback)]() mutable {
                    readDaemonSessionCatalogPage(epoch, next, std::move(callback), next.isEmpty());
                });
        });
}

void CodexAppServer::refreshDaemonSessions() {
    daemonSessionListInFlight_ = true;
    restoringDaemonSessions_.clear();
    daemonRestoreQueue_.clear();
    activeDaemonRestoreSession_.clear();
    setRestoring(true);
    readDaemonSessionCatalogPage(
        ++daemonSessionListEpoch_, {}, [this](const QJsonObject& result, const QString& error) {
            daemonSessionListInFlight_ = false;
            QTimer::singleShot(0, this, &CodexAppServer::discoverDaemonSessions);
            if (!error.isEmpty()) {
                setRestoring(false);
                return;
            }
            daemonHasByteBoundedReplay_ =
                result.value(QStringLiteral("replay_page_max_bytes")).toInteger() > 0;
            daemonHasTranscriptWindow_ = result.value("transcript_window_version").toInt() == 1;
            daemonHasTranscriptAfterCursor_ =
                result.value("transcript_window_after_cursor").toBool(false);
            daemonTranscriptWindowMaxTurns_ =
                std::clamp(result.value("transcript_window_max_turns").toInt(3), 1, 20);
            daemonHasLargeEvents_ = result.value("transcript_large_event_version").toInt() == 1;
            QStringList sessionIds;
            applyDaemonTombstones(result.value("deleted_sessions").toArray());
            for (const QJsonValue& value : result.value(QStringLiteral("sessions")).toArray()) {
                const QJsonObject stored = value.toObject();
                const QString sessionId = stored.value(QStringLiteral("session_id")).toString();
                if (sessionId.isEmpty()) {
                    continue;
                }
                sessionIds.append(sessionId);
                if (!daemonHasTranscriptWindow_)
                    restoringDaemonSessions_.insert(sessionId);
                auto& session = daemonSessions_[sessionId];
                const auto nextGeneration = stored.value("session_generation").toInteger(1);
                if (session.generation != nextGeneration) {
                    session.windowCursor = 0;
                    session.deferredCursor = 0;
                    for (auto it = daemonApprovals_.begin(); it != daemonApprovals_.end();) {
                        if (it->sessionId == sessionId)
                            it = daemonApprovals_.erase(it);
                        else
                            ++it;
                    }
                }
                session.cwd =
                    normalizedLocalPath(stored.value(QStringLiteral("worktree_path")).toString());
                session.contextRevision = stored.value("context_revision").toInteger(1);
                const auto effective = stored.value("effective").toObject();
                if (!effective.isEmpty()) {
                    session.options.insert("model", effective.value("model"));
                    session.options.insert("effort", effective.value("effort"));
                }
                session.generation =
                    stored.value(QStringLiteral("session_generation")).toInteger(1);
                if (daemonHasTranscriptWindow_) {
                    session.windowReady = false;
                    session.cursor = stored.value("last_event_cursor").toInteger();
                }
                emit daemonSessionDiscovered(stored);
                emit notificationReceived(
                    QStringLiteral("thread/backgroundTasks/updated"),
                    {{QStringLiteral("threadId"), sessionId},
                     {QStringLiteral("tasks"), stored.value(QStringLiteral("background_tasks"))}});
                if (!daemonHasTranscriptWindow_)
                    emit daemonSessionRestoreStarted(sessionId);
            }
            if (daemonHasTranscriptWindow_) {
                setRestoring(false);
                return;
            }
            daemonRestoreQueue_ = sessionIds;
            reorderDaemonRestoreQueue();
            restoreNextDaemonSession();
            if (restoringDaemonSessions_.isEmpty()) {
                setRestoring(false);
            }
        });
}

void CodexAppServer::reorderDaemonRestoreQueue() {
    if (daemonRestoreQueue_.size() < 2) {
        return;
    }
    QStringList exact;
    QStringList sameWorktree;
    QStringList background;
    for (const QString& sessionId : std::exchange(daemonRestoreQueue_, {})) {
        if (!daemonRestorePrioritySession_.isEmpty() &&
            sessionId == daemonRestorePrioritySession_) {
            exact.append(sessionId);
        } else if (!daemonRestorePriorityPath_.isEmpty() &&
                   daemonSessions_.value(sessionId).cwd == daemonRestorePriorityPath_) {
            sameWorktree.append(sessionId);
        } else {
            background.append(sessionId);
        }
    }
    daemonRestoreQueue_ = exact + sameWorktree + background;
}

void CodexAppServer::restoreNextDaemonSession() {
    if (daemonSessionListInFlight_ || !activeDaemonRestoreSession_.isEmpty()) {
        return;
    }
    while (!daemonRestoreQueue_.isEmpty()) {
        const QString sessionId = daemonRestoreQueue_.takeFirst();
        if (!restoringDaemonSessions_.contains(sessionId) || !daemonSessions_.contains(sessionId)) {
            restoringDaemonSessions_.remove(sessionId);
            continue;
        }
        activeDaemonRestoreSession_ = sessionId;
        attachDaemonSession(sessionId);
        return;
    }
    if (restoringDaemonSessions_.isEmpty()) {
        setRestoring(false);
    }
}

void CodexAppServer::attachDaemonSession(const QString& sessionId) {
    if (restoringDaemonSessions_.contains(sessionId) && activeDaemonRestoreSession_ != sessionId) {
        return;
    }
    auto found = daemonSessions_.find(sessionId);
    if (found == daemonSessions_.end() || found->replayRequestInFlight) {
        return;
    }
    if (!found->replaying) {
        found->replaying = true;
        found->replayCursor = found->cursor;
    }
    found->replayRequestInFlight = true;
    const qint64 after = found->replayCursor;
    daemon_->call(
        QStringLiteral("session.attach"),
        {{QStringLiteral("after_cursor"), after},
         {QStringLiteral("compact_completed"), true},
         {QStringLiteral("limit"),
          daemonHasByteBoundedReplay_ ? restoreReplayBatchSize : legacyRestoreReplayBatchSize}},
        sessionId, found->generation,
        [this, sessionId](const QJsonObject& result, const QString& error) {
            auto found = daemonSessions_.find(sessionId);
            if (found == daemonSessions_.end()) {
                finishDaemonReplay(sessionId);
                return;
            }
            found->replayRequestInFlight = false;
            if (!error.isEmpty()) {
                const bool pruned = result.value(QStringLiteral("error_code")).toString() ==
                                        QStringLiteral("cursor_expired") ||
                                    result.value(QStringLiteral("events_pruned")).toBool();
                if (pruned) {
                    // Retention removed events after our cursor. Replaying from it would leave a
                    // silent hole, so drop the stale cursor and any events held behind it; the
                    // session manager reloads the transcript from scratch.
                    const qint64 earliest =
                        result.value(QStringLiteral("earliest_available_cursor")).toInteger(1);
                    found->cursor = qMax<qint64>(0, earliest - 1);
                    found->replayCursor = found->cursor;
                    found->deferredEvents.clear();
                    found->deferredBytes = 0;
                    found->windowReady = false;
                }
                finishDaemonReplay(sessionId);
                emit daemonReplayFailed(sessionId, error, pruned);
                return;
            }
            const QJsonArray events = result.value(QStringLiteral("events")).toArray();
            qint64 pageCursor = found->replayCursor;
            for (const QJsonValue& value : events) {
                const QJsonObject event = value.toObject();
                pageCursor =
                    qMax(pageCursor, event.value(QStringLiteral("cursor")).toInteger(pageCursor));
                dispatchDaemonEvent(
                    {{QStringLiteral("session_id"), event.value(QStringLiteral("session_id"))},
                     {QStringLiteral("session_generation"),
                      event.value(QStringLiteral("session_generation"))},
                     {QStringLiteral("cursor"), event.value(QStringLiteral("cursor"))},
                     {QStringLiteral("payload"),
                      QJsonObject{
                          {QStringLiteral("event_type"), event.value(QStringLiteral("event_type"))},
                          {QStringLiteral("data"), event.value(QStringLiteral("payload"))}}}});
            }
            found = daemonSessions_.find(sessionId);
            if (found == daemonSessions_.end()) {
                finishDaemonReplay(sessionId);
                return;
            }
            // An active daemon from an older product build does not return the
            // compact-replay cursor. Fall back to the final event in its page
            // so upgrade deferral cannot leave Desktop in an attach loop.
            found->replayCursor =
                qMax(found->replayCursor,
                     result.value(QStringLiteral("next_event_cursor")).toInteger(pageCursor));
            found->cursor = qMax(found->cursor, found->replayCursor);
            if (found->replayCursor <
                result.value(QStringLiteral("last_event_cursor")).toInteger()) {
                QTimer::singleShot(0, this, [this, sessionId] { attachDaemonSession(sessionId); });
            } else {
                finishDaemonReplay(sessionId);
            }
        });
}

} // namespace ketplus
