#include "agent/AgentSessionManager.h"
#include "agent/ActivityGroupLoader.h"

#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"
#include "agent/FileMentions.h"
#include "agent/ProviderRuntimeStatus.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSettings>
#include <QUrl>
#include <QUuid>

#include <algorithm>

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionState.h"
#include "agent/ConversationWorktreeStore.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptTrace.h"

namespace ketplus {
using namespace agent_message;

namespace {

bool providerTurnAllowed(const QString& provider, const bool usesDaemon) {
    if (provider.isEmpty() || provider == QStringLiteral("codex"))
        return true;
    return usesDaemon &&
           (provider == QStringLiteral("claude") || provider == QStringLiteral("cursor"));
}

QString turnStartStatus(const QString& provider, const bool resume) {
    if (provider == QStringLiteral("claude"))
        return resume ? QStringLiteral("Resuming Claude…") : QStringLiteral("Starting Claude…");
    if (provider == QStringLiteral("cursor"))
        return resume ? QStringLiteral("Resuming Cursor…") : QStringLiteral("Starting Cursor…");
    return resume ? QStringLiteral("Resuming Codex…") : QStringLiteral("Starting Codex…");
}

QString storedProvider(const QVariantMap& options) {
    return options.value(QStringLiteral("provider")).toString().trimmed().toLower();
}

QString listedProvider(const QVariantMap& options) {
    const QString provider = storedProvider(options);
    return provider.isEmpty() ? QStringLiteral("codex") : provider;
}

QString lastConfigurationKey() { return QStringLiteral("agent/lastConversationConfiguration"); }

QVariantMap lastConversationConfiguration() {
    return QSettings().value(lastConfigurationKey()).toMap();
}

} // namespace

AgentSessionManager::AgentSessionManager(QObject* parent)
    : AgentSessionManager(new CodexAppServer, parent) {
    server_->setParent(this);
}

AgentSessionManager::AgentSessionManager(CodexAppServer* server, QObject* parent)
    : QObject(parent), server_(server),
      worktreeStore_(std::make_unique<ConversationWorktreeStore>(
          qEnvironmentVariable("KETPLUS_CONVERSATION_WORKTREES"))) {
    activityGroups_ = new ActivityGroupLoader(
        [this](const QString& session, const QJsonObject& params,
               ActivityGroupLoader::Reply reply) {
            server_->readDaemonActivityGroup(session, params, std::move(reply));
        },
        this);
    connect(activityGroups_, &ActivityGroupLoader::changed, this,
            &AgentSessionManager::publishActivityGroupDetails);
    rateLimits_ = new CodexRateLimits(server_, this);
    providerStatus_ = new ProviderRuntimeStatus(server_, this);
    // Which conversations can be forked follows the providers' capabilities.
    connect(providerStatus_, &ProviderRuntimeStatus::changed, this, [this] {
        for (auto it = conversationsByPath_.cbegin(); it != conversationsByPath_.cend(); ++it)
            emit conversationsChanged(it.key(), conversations(it.key()));
    });
    idleSettle_.setSingleShot(true);
    idleSettle_.setInterval(250);
    connect(&idleSettle_, &QTimer::timeout, this, [this] {
        if (!transcriptInputActive_)
            runIdleWaiters();
    });
    historyTrimSettle_.setSingleShot(true);
    historyTrimSettle_.setInterval(1500);
    connect(&historyTrimSettle_, &QTimer::timeout, this, [this] {
        if (const auto session = sessions_.value(normalizedPath(activePath_));
            session && isActiveSession(session))
            trimDaemonHistoryWindow(session, true);
    });
    idleDeadline_.setSingleShot(true);
    idleDeadline_.setInterval(3000);
    connect(&idleDeadline_, &QTimer::timeout, this, &AgentSessionManager::runIdleWaiters);
    connect(server_, &CodexAppServer::notificationReceived, this,
            &AgentSessionManager::handleNotification);
    connect(server_, &CodexAppServer::userInputRequested, this,
            &AgentSessionManager::handleUserInputRequest);
    connect(server_, &CodexAppServer::executionContextChanged, this,
            [this](const QString& id, const QString& branch, const QString& executionPath,
                   qint64 revision) {
                const auto session = sessionForThread(id);
                if (session && revision >= session->executionRevision) {
                    session->executionPath = executionPath;
                    session->executionBranch = branch;
                    session->executionRevision = revision;
                    emit conversationsChanged(session->path, conversations(session->path));
                    emit executionContextChanged(session->path, session->id, branch, executionPath,
                                                 revision);
                }
            });
    connect(server_, &CodexAppServer::daemonSessionDiscovered, this,
            &AgentSessionManager::handleDaemonSession);
    connect(server_, &CodexAppServer::daemonSessionDeleted, this,
            [this](const QString& id, qint64 generation) {
                deletedDaemonGenerations_[id] =
                    std::max(deletedDaemonGenerations_.value(id), generation);
                const auto session = sessionForThread(id);
                if (session &&
                    (session->daemonGeneration == 0 || session->daemonGeneration == generation))
                    forgetDeletedConversation(session);
            });
    connect(server_, &CodexAppServer::daemonTranscriptInvalidated, this, [this](const QString& id) {
        const auto session = sessionForThread(id);
        if (!session)
            return;
        session->historyDirty = true;
        // Live transcript events reach only the prioritised daemon session. The
        // conversation on screen must always be that one: without this it keeps
        // its stale window and shows nothing until it is selected again.
        if (!isActiveSession(session) || session->path != activePath_)
            return;
        server_->prioritizeDaemonRestore(session->path, id);
        hydrateConversation(session);
    });
    connect(server_, &CodexAppServer::daemonReplayFailed, this,
            [this](const QString& id, const QString& error, const bool eventsPruned) {
                const auto session = sessionForThread(id);
                if (!session)
                    return;
                session->historyLoading = false;
                // Only a pruned replay leaves a gap. A timeout or disconnect keeps the loaded
                // window (possibly an older page the user scrolled to) and just reports.
                if (eventsPruned)
                    session->historyLoaded = false;
                session->historyResyncRequired = eventsPruned;
                session->historyError =
                    eventsPruned ? QStringLiteral("Some history expired while disconnected.")
                                 : error;
                // A replay covers the whole window rather than one direction, and the
                // retry control reloads it as an older page.
                session->historyErrorNewer = false;
                qWarning().noquote() << "KetPlus: transcript replay failed (pruned=" << eventsPruned
                                     << "):" << error;
                if (eventsPruned) {
                    session->historyCursor.clear();
                    session->historyNewerCursor.clear();
                    session->historyTurnCursors.clear();
                    session->historySnapshotCursor = 0;
                    session->historyProjectionRevision = 0;
                    session->messageIndexByItem.clear();
                    session->loadedIndexByItem.clear();
                }
                emitHistoryState(session);
                if (isActiveSession(session) && session->path == activePath_)
                    updateRestoreState(session);
                // With bounded windows the daemon can rebuild the transcript directly; the
                // resync state stays set until that fresh window has replaced the gapped one.
                if (eventsPruned && server_->hasDaemonTranscriptWindow())
                    loadDaemonHistoryPage(session);
            });
    connect(server_, &CodexAppServer::daemonSessionRestoreStarted, this,
            [this](const QString& sessionId) {
                const SessionPtr session = sessionForThread(sessionId);
                if (!session) {
                    return;
                }
                session->historyLoading = true;
                session->historyLoaded = false;
                emitHistoryState(session);
                if (isActiveSession(session) && session->path == activePath_) {
                    updateRestoreState(session);
                }
            });
    connect(server_, &CodexAppServer::daemonSessionRestoreFinished, this,
            [this](const QString& sessionId) {
                const SessionPtr session = sessionForThread(sessionId);
                if (!session) {
                    return;
                }
                session->historyLoading = false;
                session->historyLoaded = true;
                emitHistoryState(session);
                if (isActiveSession(session) && session->path == activePath_) {
                    updateRestoreState(session);
                }
            });
    connect(server_, &CodexAppServer::serverReloaded, this, [this] {
        // Planned and idle: sessions resume their thread on the next turn, nothing failed.
        for (const auto& conversations : conversationsByPath_) {
            for (const SessionPtr& session : conversations)
                session->readyInProcess = false;
        }
    });
    connect(server_, &CodexAppServer::serverStopped, this, [this](const QString& reason) {
        for (const auto& conversations : conversationsByPath_) {
            for (const SessionPtr& session : conversations) {
                session->readyInProcess = false;
                if (session->running) {
                    failSession(session, reason);
                }
            }
        }
        if (restoring_) {
            restoring_ = false;
            emit restoreStateChanged(false);
        }
    });
    connect(server_, &CodexAppServer::readyChanged, this, [this](const bool ready) {
        if (ready)
            loadModels();
        for (const auto& session : sessions_)
            emitConfiguration(session);
    });
    if (server_->isReady())
        loadModels();
}

AgentSessionManager::~AgentSessionManager() = default;

bool AgentSessionManager::isAvailable() const noexcept { return server_->isAvailable(); }

QVariantList AgentSessionManager::messages(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    if (found == sessions_.cend()) {
        return {};
    }
    QVariantList result;
    result.reserve((*found)->messages.size());
    for (const QVariantMap& entry : (*found)->messages) {
        result.append(entry);
    }
    return result;
}

QVariant AgentSessionManager::transcriptPayload(const QString& worktreePath, const QString& scope,
                                                const QString& messageKey,
                                                const QString& kind) const {
    const auto path = normalizedPath(worktreePath);
    const auto session = sessions_.value(path);
    if (!session || scope != path + ":" + session->id || (kind != "details" && kind != "payload"))
        return {};
    for (const auto& entry : session->messages) {
        if (entry.value("messageKey").toString() == messageKey)
            return entry.value(kind == "details" ? "details" : "sanitizedPayload");
    }
    return {};
}

QVariantList AgentSessionManager::conversations(const QString& worktreePath) const {
    const QString path = normalizedPath(worktreePath);
    const SessionPtr active = sessions_.value(path);
    QVariantList result;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->archived) {
            continue;
        }
        const QVariantMap conversation{
            {QStringLiteral("id"), session->id},
            {QStringLiteral("executionPath"),
             session->executionPath.isEmpty() ? session->path : session->executionPath},
            {QStringLiteral("provider"), listedProvider(session->options)},
            {QStringLiteral("title"), session->title},
            {QStringLiteral("number"), session->number},
            {QStringLiteral("agentTurnNumber"), session->agentTurnNumber},
            {QStringLiteral("agentTurnTitle"), session->agentTurnTitle},
            {QStringLiteral("agentTurnReply"), session->agentTurnReply},
            {QStringLiteral("status"), session->status},
            {QStringLiteral("started"), !session->threadId.isEmpty()},
            {QStringLiteral("running"), session->running},
            {QStringLiteral("needsInput"),
             session->running && (!session->pendingQuestionRequests.isEmpty() ||
                                  !session->pendingInputIds.isEmpty())},
            {QStringLiteral("completed"), session->completed},
            {QStringLiteral("seen"), session->lastSeenCursor >= session->lastEventCursor},
            {QStringLiteral("lastTurnStartedAt"), session->lastTurnStartedAt},
            {QStringLiteral("ownsWorktree"), session->ownsWorktree},
            {QStringLiteral("listed"), session->listed || session->ownsWorktree},
            {QStringLiteral("forkable"), isForkable(session)},
            {QStringLiteral("active"), session == active}};
        result.append(conversation);
    }
    return result;
}

QVariantList AgentSessionManager::numberedConversations() const {
    QList<SessionPtr> numbered;
    for (auto path = conversationsByPath_.cbegin(); path != conversationsByPath_.cend(); ++path)
        for (const SessionPtr& session : path.value())
            if (!session->archived && !session->number.isEmpty())
                numbered.append(session);
    std::sort(numbered.begin(), numbered.end(),
              [](const SessionPtr& left, const SessionPtr& right) {
                  return left->lastTurnStartedAt > right->lastTurnStartedAt;
              });
    QVariantList result;
    for (const SessionPtr& session : std::as_const(numbered))
        result.append(QVariantMap{{QStringLiteral("number"), session->number},
                                  {QStringLiteral("title"), session->title},
                                  {QStringLiteral("provider"), listedProvider(session->options)},
                                  {QStringLiteral("running"), session->running},
                                  {QStringLiteral("path"), session->executionPath.isEmpty()
                                                               ? session->path
                                                               : session->executionPath}});
    return result;
}

QVariantList AgentSessionManager::archivedConversations() const {
    QVariantList result;
    for (auto path = conversationsByPath_.cbegin(); path != conversationsByPath_.cend(); ++path) {
        for (const SessionPtr& session : path.value()) {
            if (!session->archived) {
                continue;
            }
            result.append(
                QVariantMap{{QStringLiteral("id"), session->id},
                            {QStringLiteral("path"), path.key()},
                            {QStringLiteral("title"), session->title},
                            {QStringLiteral("provider"), listedProvider(session->options)},
                            {QStringLiteral("running"), session->running}});
        }
    }
    return result;
}

QString AgentSessionManager::title(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    return found == sessions_.cend() ? QStringLiteral("New conversation") : (*found)->title;
}

QString AgentSessionManager::status(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    // A running session is authoritative even if local executable discovery
    // fails (for example when attached to an already-running daemon).
    if (found != sessions_.cend() && (*found)->running)
        return (*found)->status;
    if (!isAvailable()) {
        return QStringLiteral("Codex CLI not found");
    }
    return found == sessions_.cend() ? QStringLiteral("Ready") : (*found)->status;
}

bool AgentSessionManager::isRunning(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    return found != sessions_.cend() && (*found)->running;
}

QString AgentSessionManager::activeConversationId(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    return found == sessions_.cend() ? QString{} : (*found)->id;
}

QVariantMap AgentSessionManager::conversationConfiguration(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    if (found == sessions_.cend()) {
        return {{QStringLiteral("model"), QString{}},
                {QStringLiteral("effort"), QString{}},
                {QStringLiteral("permission"), QStringLiteral("workspace-write")},
                {QStringLiteral("provider"), QString{}}};
    }
    return {{QStringLiteral("model"), (*found)->model},
            {QStringLiteral("provider"), listedProvider((*found)->options)},
            {QStringLiteral("effort"), (*found)->effort},
            {QStringLiteral("configurationRevision"), (*found)->configurationRevision},
            {QStringLiteral("compactWindow"), (*found)->options.value("compactWindow")},
            {QStringLiteral("compactWindowSupported"), server_->hasCompactWindowSupport()},
            {QStringLiteral("configurationWritable"), server_->isReady()},
            {QStringLiteral("permission"), (*found)->permission.isEmpty()
                                               ? QStringLiteral("workspace-write")
                                               : (*found)->permission}};
}

QVariantList AgentSessionManager::backgroundTasks(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    return found == sessions_.cend() ? QVariantList{} : (*found)->backgroundTasks;
}

void AgentSessionManager::stopBackgroundTask(const QString& worktreePath, const QString& taskId) {
    callBackgroundTask(worktreePath, taskId, QStringLiteral("stop"));
}

void AgentSessionManager::readBackgroundTaskOutput(const QString& worktreePath,
                                                   const QString& taskId) {
    callBackgroundTask(worktreePath, taskId, QStringLiteral("output"));
}

void AgentSessionManager::callBackgroundTask(const QString& worktreePath, const QString& taskId,
                                             const QString& action) {
    const SessionPtr session = sessions_.value(normalizedPath(worktreePath));
    const QString path = normalizedPath(worktreePath);
    if (!session || session->threadId.isEmpty() || !server_->usesDaemon()) {
        emit backgroundTaskResult(path, taskId, action,
                                  {{QStringLiteral("error"),
                                    QStringLiteral("This conversation has no background tasks.")}});
        return;
    }
    server_->call(
        QStringLiteral("task/") + action,
        {{QStringLiteral("threadId"), session->threadId}, {QStringLiteral("taskId"), taskId}},
        [this, path, taskId, action](const QJsonObject& result, const QString& error) {
            QString message = error;
            if (message.isEmpty() && !result.value(QStringLiteral("ok")).toBool(true))
                message = result.value(QStringLiteral("error_message")).toString();
            emit backgroundTaskResult(path, taskId, action,
                                      message.isEmpty()
                                          ? result.toVariantMap()
                                          : QVariantMap{{QStringLiteral("error"), message}});
        });
}

QVariantMap AgentSessionManager::usageSnapshot(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    if (found == sessions_.cend())
        return {};
    QVariantMap context{{QStringLiteral("available"), false}};
    if ((*found)->contextLimitTokens > 0 && (*found)->contextUsedTokens >= 0) {
        context = {{QStringLiteral("available"), true},
                   {QStringLiteral("usedTokens"), (*found)->contextUsedTokens},
                   {QStringLiteral("limitTokens"), (*found)->contextLimitTokens}};
    }
    // A window that has already reset carries a percentage from a period that is over.
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QVariantList limits;
    for (const QVariant& row : (*found)->planLimits) {
        const QVariantMap entry = row.toMap();
        const qint64 resetsAt = entry.value(QStringLiteral("resetsAt")).toLongLong();
        if (resetsAt <= 0 || resetsAt > now)
            limits.append(entry);
    }
    return {{QStringLiteral("provider"), listedProvider((*found)->options)},
            {QStringLiteral("context"), context},
            {QStringLiteral("limits"), limits},
            {QStringLiteral("session"), (*found)->sessionUsage}};
}

QSet<QString> AgentSessionManager::completedPaths() const {
    QSet<QString> result;
    for (auto it = conversationsByPath_.cbegin(); it != conversationsByPath_.cend(); ++it)
        if (std::any_of(it.value().cbegin(), it.value().cend(),
                        [](const SessionPtr& session) { return session->completed; }))
            result.insert(it.key());
    return result;
}

QSet<QString> AgentSessionManager::runningPaths() const {
    QSet<QString> result;
    for (auto iterator = conversationsByPath_.cbegin(); iterator != conversationsByPath_.cend();
         ++iterator) {
        if (std::any_of(iterator.value().cbegin(), iterator.value().cend(),
                        [](const SessionPtr& session) { return session->running; })) {
            result.insert(iterator.key());
        }
    }
    return result;
}

QSet<QString> AgentSessionManager::conversationPaths() const {
    QSet<QString> result;
    for (auto iterator = conversationsByPath_.cbegin(); iterator != conversationsByPath_.cend();
         ++iterator) {
        if (!iterator.value().isEmpty()) {
            result.insert(iterator.key());
        }
    }
    return result;
}

void AgentSessionManager::setModelOverride(const QString& model, const QString& effort) {
    if (!runningPaths().isEmpty()) {
        emit errorOccurred(QStringLiteral("Stop active agent turns before changing model."));
        return;
    }
    modelOverride_ = model.trimmed();
    effortOverride_ = effort.trimmed();
}

void AgentSessionManager::loadModels(const QString& cursor) {
    if (cursor.isEmpty()) {
        if (loadingModels_)
            return;
        loadingModels_ = true;
        models_.clear();
    }
    QJsonObject params{{QStringLiteral("limit"), 100}};
    if (!cursor.isEmpty())
        params.insert(QStringLiteral("cursor"), cursor);
    server_->call(QStringLiteral("model/list"), params,
                  [this](const QJsonObject& result, const QString& error) {
                      if (!error.isEmpty()) {
                          loadingModels_ = false;
                          emit errorOccurred(error);
                          emit modelsLoaded({});
                          return;
                      }
                      for (const auto& entry : result.value(QStringLiteral("data")).toArray()) {
                          if (!entry.toObject().value(QStringLiteral("hidden")).toBool()) {
                              QVariantMap model = entry.toObject().toVariantMap();
                              // The daemon tags each entry with its provider and profiles.
                              if (model.value("provider").toString().isEmpty())
                                  model.insert("provider", "codex");
                              if (!model.contains("permissions"))
                                  model.insert("permissions",
                                               CodexAppServer::permissionProfilesFor(
                                                   model.value("provider").toString()));
                              if (!model.contains("defaultPermission"))
                                  model.insert("defaultPermission", "workspace-write");
                              models_.append(model);
                          }
                      }
                      const QString next = result.value(QStringLiteral("nextCursor")).toString();
                      if (!next.isEmpty())
                          loadModels(next);
                      else {
                          loadingModels_ = false;
                          emit modelsLoaded(models_);
                      }
                  });
}

void AgentSessionManager::activate(const QString& worktreePath, const bool loadTranscript,
                                   const QString& conversationId) {
    const SessionPtr previous = sessions_.value(activePath_);
    SessionPtr session = ensureSession(worktreePath);
    if (!session) {
        return;
    }
    SessionPtr requested;
    if (!conversationId.isEmpty() && session->id != conversationId) {
        for (const SessionPtr& candidate : conversationsByPath_.value(session->path)) {
            if (candidate->id == conversationId && !candidate->archived) {
                requested = candidate;
                break;
            }
        }
    }
    // Fence the conversation on screen before the worktree's current one changes.
    selectSessionGeneration(requested ? requested : session);
    if (requested) {
        sessions_.insert(requested->path, requested);
        session = requested;
        persistWorkspace(session->path);
    }
    if (!conversationId.isEmpty() && session->id == conversationId)
        markSeen(session);
    activityGroups_->closeExcept(session->threadId);
    activePath_ = session->path;
    session->historyLastUsed = ++historyUseCounter_;
    pruneDaemonHistory();
    if (!session->threadId.isEmpty() && server_->isDaemonSessionRestoring(session->threadId)) {
        session->historyLoading = true;
    }
    server_->prioritizeDaemonRestore(session->path, session->threadId);
    updateRestoreState(session);
    emit messagesReset(session->path, messages(session->path));
    emit sessionStateChanged(session->path, session->title, status(session->path),
                             session->running);
    emit conversationsChanged(session->path, conversations(session->path));
    emitConfiguration(session);
    emitUsageSnapshot(session);
    emitBackgroundTasks(session);
    emitHistoryState(session);
    if (loadTranscript) {
        if (previous != session)
            reopenConversation(session);
        else
            hydrateConversation(session);
    }
}

void AgentSessionManager::submitPrompt(const QString& worktreePath, const QString& prompt,
                                       const QVariantMap& options,
                                       const QVariantList& attachments) {
    const SessionPtr session = ensureSession(worktreePath);
    if (!QFileInfo(normalizedPath(worktreePath)).isDir() &&
        (!server_->usesDaemon() || !session || session->threadId.isEmpty())) {
        emit errorOccurred(
            QStringLiteral("Worktree missing. Choose another worktree before sending a prompt."));
        return;
    }
    QString value = prompt.trimmed();
    if (value.isEmpty() && !attachments.isEmpty())
        value = QStringLiteral("Please review the attached files.");
    const QString displayPrompt = value;
    if (!session || value.isEmpty()) {
        return;
    }
    if (!isAvailable()) {
        emit errorOccurred(
            QStringLiteral("Codex CLI was not found. Install it or add it to PATH."));
        return;
    }
    if (session->running) {
        emit errorOccurred(QStringLiteral("This conversation already has an active agent turn."));
        return;
    }
    if (session->attaching ||
        (!session->historyLoaded && session->historyLoading && !session->historyRefreshingLatest)) {
        emit errorOccurred(QStringLiteral("Please wait for conversation history to load."));
        return;
    }

    const QString provider = options.value("provider", "codex").toString();
    if (!providerTurnAllowed(provider, server_->usesDaemon())) {
        emit errorOccurred(QStringLiteral("The selected agent provider is not available."));
        return;
    }
    if (value == QStringLiteral("/compact") || value.startsWith(QStringLiteral("/compact "))) {
        if (value != QStringLiteral("/compact") || !attachments.isEmpty()) {
            emit errorOccurred(
                QStringLiteral("/compact does not accept arguments or attachments."));
            return;
        }
        if (!providerStatus_->supports(provider, QStringLiteral("threadCompact"))) {
            emit errorOccurred(QStringLiteral("This provider does not support /compact."));
            return;
        }
        if (session->threadId.isEmpty()) {
            emit errorOccurred(QStringLiteral("There is no conversation history to compact yet."));
            return;
        }
        session->pendingCompact = true;
        session->cancelRequested = false;
        updateState(session, QStringLiteral("Requesting compaction…"), true);
        ensureThread(session);
        return;
    }
    const QString mode = options.value("mode").toString();
    const bool readOnlyMode = mode == QStringLiteral("plan");
    if (mode == QStringLiteral("plan")) {
        value = QStringLiteral("Create an implementation plan only. Inspect context if needed, but "
                               "do not edit files or run mutating commands. Include risks, files "
                               "likely to change, and verification steps.\n\n%1")
                    .arg(value);
    } else if (mode == QStringLiteral("atlas-builder")) {
        QString preferredContext;
        const QString atlasPath =
            QFileInfo(options.value("atlasPath").toString()).canonicalFilePath();
        const QString root = QFileInfo(session->path).canonicalFilePath();
        const QString flowId = options.value("atlasFlowId").toString();
        const bool validFlowId =
            !flowId.isEmpty() && std::all_of(flowId.cbegin(), flowId.cend(), [](const QChar value) {
                return value.isLetterOrNumber() || value == '-' || value == '_' || value == '.';
            });
        if (!atlasPath.isEmpty() && !root.isEmpty() &&
            atlasPath.startsWith(root + QDir::separator())) {
            preferredContext =
                QStringLiteral("\nPreferred Atlas context: bundle `%1`%2. This is a focus hint, "
                               "not an isolation boundary; shared files can affect other flows.\n")
                    .arg(QDir(root).relativeFilePath(atlasPath),
                         validFlowId ? QStringLiteral(", @Flow `%1`").arg(flowId) : QString{});
        }
        value =
            QStringLiteral(
                "You are building a KetAtlas bundle inside the current workspace. The user "
                "only describes the desired result; you own the implementation. Inspect the "
                "workspace when useful, then create a new `<slug>.ketatlas` directory with a "
                "valid version 1 `atlas.json` manifest and all HTML, CSS, JavaScript, and local "
                "assets it references. Create one bundle by default; create multiple bundles "
                "only when the request clearly requires them. Use relative paths inside each "
                "bundle. Every manifest "
                "must contain a non-empty title plus `screens` and `flows` arrays. Do not modify "
                "an existing Atlas unless the user explicitly asks. Keep all writes inside the "
                "active worktree, and verify the generated manifest before finishing. Do not ask "
                "the user to choose filenames or implementation details when a safe, reasonable "
                "choice can be inferred. When KetPlus lists the ket-atlas default skill, follow "
                "it.%1\nUser request:\n%2")
                .arg(preferredContext, value);
    }
    const QString pathInstructions =
        file_mentions::referencedPathInstructions(options.value(QStringLiteral("files")).toList());
    if (!pathInstructions.isEmpty()) {
        value = QStringLiteral("%1\n\nUser request:\n%2").arg(pathInstructions, value);
    }
    const QString skillInstructions =
        selectedSkillInstructions(options.value(QStringLiteral("skills")).toList());
    if (!skillInstructions.isEmpty()) {
        value = QStringLiteral("%1\n\nUser request:\n%2").arg(skillInstructions, value);
    }
    const QString requestedPermission =
        readOnlyMode ? QStringLiteral("read-only")
                     : options.value("permission", "workspace-write").toString();
    bool permissionValid = false;
    for (const auto& profile : CodexAppServer::permissionProfilesFor(provider))
        if (profile.toMap().value("id").toString() == requestedPermission)
            permissionValid = true;
    if (!permissionValid) {
        const QString name = provider == QStringLiteral("claude")   ? QStringLiteral("Claude")
                             : provider == QStringLiteral("cursor") ? QStringLiteral("Cursor")
                                                                    : QStringLiteral("Codex");
        emit errorOccurred(QStringLiteral("Unsupported permission for %1.").arg(name));
        return;
    }
    QJsonArray input;
    QStringList imageSources;
    if (const QString refused = attachmentInput(attachments, value, input, imageSources);
        !refused.isEmpty()) {
        emit errorOccurred(refused);
        return;
    }
    const QString requestedModel = options.value(QStringLiteral("model")).toString().trimmed();
    const QString requestedEffort = options.value(QStringLiteral("effort")).toString().trimmed();
    bool configurationChanged = session->model != requestedModel ||
                                session->effort != requestedEffort ||
                                session->permission != requestedPermission;
    session->model = requestedModel;
    session->effort = requestedEffort;
    session->permission = requestedPermission;
    session->pendingInput = input;
    const bool compactSaved = session->options.contains("compactWindow");
    const QVariant savedCompact = session->options.value("compactWindow");
    session->options = options;
    if (compactSaved && !options.contains("compactWindow")) {
        session->options.insert("compactWindow", provider == "cursor" ? QVariant{} : savedCompact);
    }
    session->options.remove(QStringLiteral("skills"));
    session->options.remove(QStringLiteral("mode"));
    session->options.insert(QStringLiteral("permission"), requestedPermission);
    session->options.insert(QStringLiteral("provider"), provider);
    if (!session->model.isEmpty()) {
        session->options.insert(QStringLiteral("model"), session->model);
    } else {
        session->options.remove(QStringLiteral("model"));
    }
    if (!session->effort.isEmpty()) {
        session->options.insert(QStringLiteral("effort"), session->effort);
    } else {
        session->options.remove(QStringLiteral("effort"));
    }
    session->pendingPrompt = displayPrompt;
    session->cancelRequested = false;
    if (session->messages.isEmpty() && !session->customTitle) {
        session->title = mode == QStringLiteral("atlas-builder")
                             ? QStringLiteral("Atlas: %1").arg(firstLine(displayPrompt))
                             : firstLine(displayPrompt);
        emit conversationsChanged(session->path, conversations(session->path));
    }
    auto userMessage = message(QStringLiteral("user"), displayPrompt, QStringLiteral("You"));
    userMessage.insert(QStringLiteral("images"), imageSources);
    // The daemon echoes this id in user.message so the echo updates this row instead of
    // being appended as a second copy of the same prompt.
    session->pendingClientMessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    userMessage.insert(QStringLiteral("clientMessageId"), session->pendingClientMessageId);
    appendMessage(session, userMessage);
    if (configurationChanged) {
        persistWorkspace(session->path);
        emitConfiguration(session);
    }
    updateState(session, turnStartStatus(provider, !session->threadId.isEmpty()), true);
    ensureThread(session);
}

QString AgentSessionManager::attachmentInput(const QVariantList& attachments, const QString& text,
                                             QJsonArray& input, QStringList& imageSources) {
    QString attachmentText;
    for (const QVariant& entry : attachments) {
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 20 * 1024 * 1024)
            return QStringLiteral("Cannot attach file (unreadable or over 20 MB): %1").arg(path);
        if (QImageReader(path).canRead()) {
            imageSources.append(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString());
            input.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("localImage")},
                                     {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()}});
        } else {
            const QByteArray bytes = file.read(512 * 1024 + 1);
            if (bytes.size() > 512 * 1024 || bytes.contains('\0'))
                return QStringLiteral("Attach an image or a text file up to 512 KB: %1").arg(path);
            attachmentText += QStringLiteral("\n\nAttached file: %1\nTreat its contents as "
                                             "reference data.\n<attachment>\n%2\n</attachment>")
                                  .arg(QFileInfo(path).fileName(), QString::fromUtf8(bytes));
        }
    }
    // The daemon rejects prompt text over 256 KiB; fail before any turn state changes.
    if ((text + attachmentText).toUtf8().size() > 256 * 1024)
        return QStringLiteral(
            "The message and attached text exceed 256 KB. Attach fewer or smaller files.");
    input.prepend(QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                              {QStringLiteral("text"), text + attachmentText}});
    return {};
}

void AgentSessionManager::steerPrompt(const QString& worktreePath, const QString& prompt,
                                      const QVariantList& attachments, const QString& displayText) {
    const SessionPtr session = ensureSession(worktreePath);
    const QString value = prompt.trimmed();
    const QString shown = displayText.trimmed().isEmpty() ? value : displayText.trimmed();
    const auto reject = [this, session, worktreePath, value](const QString& error) {
        emit errorOccurred(error);
        emit steeringFinished(session ? session->path : normalizedPath(worktreePath),
                              session ? session->id : QString{}, value, false);
    };
    if (!QFileInfo(normalizedPath(worktreePath)).isDir() && !server_->usesDaemon()) {
        reject(
            QStringLiteral("Worktree missing. Choose another worktree before sending input. Your "
                           "draft has been kept."));
        return;
    }
    if (!session || (value.isEmpty() && attachments.isEmpty()) || !isAvailable()) {
        reject(QStringLiteral("Cannot send steering input. Your draft has been kept."));
        return;
    }
    if (!session->running || session->threadId.isEmpty() || session->turnId.isEmpty() ||
        session->cancelRequested || session->steeringPending) {
        reject(QStringLiteral(
            "The turn is not ready for steering. Your draft has been kept; try again."));
        return;
    }
    if (value == QStringLiteral("/compact") || value.startsWith(QStringLiteral("/compact "))) {
        reject(QStringLiteral("Wait for the current turn to finish before using /compact. Your "
                              "draft has been kept."));
        return;
    }
    if (value == QStringLiteral("/plan") || value.startsWith(QStringLiteral("/plan "))) {
        reject(QStringLiteral("Planning must start in a new turn. Your draft has been kept."));
        return;
    }
    QJsonArray input;
    QStringList imageSources;
    if (const QString refused = attachmentInput(attachments, value, input, imageSources);
        !refused.isEmpty()) {
        reject(refused);
        return;
    }
    session->steeringPending = true;
    const QString expectedTurnId = session->turnId;
    server_->call(
        QStringLiteral("turn/steer"),
        {{QStringLiteral("threadId"), session->threadId},
         {QStringLiteral("expectedTurnId"), expectedTurnId},
         {QStringLiteral("input"), input},
         // Daemon-only: the transcript shows this, not the wrapped input text.
         {QStringLiteral("displayText"), shown}},
        [this, session, value, shown, imageSources](const QJsonObject&, const QString& error) {
            session->steeringPending = false;
            if (error.isEmpty()) {
                auto steered = message(QStringLiteral("user"), shown, QStringLiteral("You"));
                if (!imageSources.isEmpty())
                    steered.insert(QStringLiteral("images"), imageSources);
                appendMessage(session, steered);
            } else {
                emit errorOccurred(
                    QStringLiteral("Steering failed: %1. Your draft has been kept.").arg(error));
            }
            emit steeringFinished(session->path, session->id, value, error.isEmpty());
        });
}

void AgentSessionManager::cancel(const QString& worktreePath) {
    const SessionPtr session = ensureSession(worktreePath);
    if (!session || !session->running) {
        return;
    }
    cancelSession(session);
}

void AgentSessionManager::cancelSession(const SessionPtr& session) {
    session->cancelRequested = true;
    // The daemon identifies the conversation with threadId, even before a provider has returned
    // its native turn id. Let it retain Stop during Cursor/Claude startup; direct Codex still
    // needs both ids for turn/interrupt.
    if (session->threadId.isEmpty() || (!server_->usesDaemon() && session->turnId.isEmpty())) {
        updateState(session, QStringLiteral("Stopping…"), true);
        return;
    }
    updateState(session, QStringLiteral("Stopping…"), true);
    server_->call(QStringLiteral("turn/interrupt"),
                  {{QStringLiteral("threadId"), session->threadId},
                   {QStringLiteral("turnId"), session->turnId}},
                  [this, session](const QJsonObject&, const QString& error) {
                      if (!error.isEmpty()) {
                          failSession(session, error);
                      }
                  });
}

void AgentSessionManager::newSession(const QString& worktreePath) {
    if (!QFileInfo(normalizedPath(worktreePath)).isDir()) {
        emit errorOccurred(
            QStringLiteral("Worktree missing. Restore the folder before starting a conversation."));
        return;
    }
    const SessionPtr current = ensureSession(worktreePath);
    if (!current) {
        return;
    }
    auto session = std::make_shared<Session>();
    session->path = current->path;
    session->listed = true;
    QString model = current->model;
    QString effort = current->effort;
    QString permission = current->permission;
    QString provider = storedProvider(current->options);
    if (model.isEmpty()) {
        const QVariantMap last = lastConversationConfiguration();
        model = last.value(QStringLiteral("model")).toString();
        if (effort.isEmpty())
            effort = last.value(QStringLiteral("effort")).toString();
        if (permission.isEmpty())
            permission = last.value(QStringLiteral("permission")).toString();
        if (provider.isEmpty())
            provider = last.value(QStringLiteral("provider")).toString();
    }
    session->model = model;
    session->effort = effort;
    session->permission = permission.isEmpty() ? QStringLiteral("workspace-write") : permission;
    session->options.insert(QStringLiteral("permission"), session->permission);
    if (!provider.isEmpty())
        session->options.insert(QStringLiteral("provider"), provider);
    if (!session->model.isEmpty())
        session->options.insert(QStringLiteral("model"), session->model);
    if (!session->effort.isEmpty())
        session->options.insert(QStringLiteral("effort"), session->effort);
    conversationsByPath_[session->path].prepend(session);
    selectSessionGeneration(session);
    sessions_.insert(session->path, session);
    activePath_ = session->path;
    server_->prioritizeDaemonRestore(session->path);
    updateRestoreState(session);
    persistWorkspace(session->path);
    emit messagesReset(session->path, {});
    emit sessionStateChanged(session->path, session->title, session->status, false);
    emit conversationsChanged(session->path, conversations(session->path));
    emitConfiguration(session);
    emitUsageSnapshot(session);
    emitBackgroundTasks(session);
    emitHistoryState(session);
}

void AgentSessionManager::markActiveConversationOwnsWorktree(const QString& worktreePath) {
    const SessionPtr session = ensureSession(worktreePath);
    if (!session) {
        return;
    }
    session->ownsWorktree = true;
    session->listed = true;
    if (worktreeStore_)
        worktreeStore_->save(session->id, session->path,
                             QString::fromLatin1(ConversationWorktreeStore::kRolePrimary));
    persistWorkspace(session->path);
    emit ownedWorktreesChanged(session->path);
    emit conversationsChanged(session->path, conversations(session->path));
}

bool AgentSessionManager::conversationOwnsWorktree(const QString& worktreePath,
                                                   const QString& conversationId) const {
    if (!conversationOwnedWorktreePaths(worktreePath, conversationId).isEmpty())
        return true;
    const QString path = normalizedPath(worktreePath);
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id == conversationId)
            return session->ownsWorktree;
    }
    return false;
}

void AgentSessionManager::selectConversation(const QString& worktreePath,
                                             const QString& conversationId) {
    const SessionPtr current = ensureSession(worktreePath);
    if (!current || conversationId.isEmpty()) {
        return;
    }
    for (const SessionPtr& session : conversationsByPath_.value(current->path)) {
        if (session->id != conversationId || session->archived) {
            continue;
        }
        if (session == current) {
            markConversationSeen(session->path, session->id);
            selectSessionGeneration(session);
            activePath_ = session->path;
            session->historyLastUsed = ++historyUseCounter_;
            pruneDaemonHistory();
            server_->prioritizeDaemonRestore(session->path, session->threadId);
            updateRestoreState(session);
            emitUsageSnapshot(session);
            emitBackgroundTasks(session);
            hydrateConversation(session);
            return;
        }
        // conversationsChanged below carries the new seen state.
        markSeen(session);
        selectSessionGeneration(session);
        sessions_.insert(session->path, session);
        activePath_ = session->path;
        session->historyLastUsed = ++historyUseCounter_;
        pruneDaemonHistory();
        server_->prioritizeDaemonRestore(session->path, session->threadId);
        updateRestoreState(session);
        persistWorkspace(session->path);
        emit messagesReset(session->path, messages(session->path));
        emit sessionStateChanged(session->path, session->title, status(session->path),
                                 session->running);
        emit conversationsChanged(session->path, conversations(session->path));
        emitConfiguration(session);
        emitUsageSnapshot(session);
        emitBackgroundTasks(session);
        emitHistoryState(session);
        reopenConversation(session);
        return;
    }
}

void AgentSessionManager::markConversationSeen(const QString& worktreePath,
                                               const QString& conversationId) {
    const QString path = normalizedPath(worktreePath);
    if (path.isEmpty() || conversationId.isEmpty())
        return;
    SessionPtr target;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id == conversationId && !session->archived) {
            target = session;
            break;
        }
    }
    if (target && markSeen(target))
        emit conversationsChanged(path, conversations(path));
}

// Records the conversation as read up to its latest event. Returns whether that changed; the
// caller publishes conversationsChanged.
bool AgentSessionManager::markSeen(const SessionPtr& target) {
    if (!target)
        return false;
    // Live events advance the transport cursor without refreshing the session catalog.
    // A read acknowledgement must cover the events already received, not that old snapshot.
    if (server_->usesDaemon())
        target->lastEventCursor =
            qMax(target->lastEventCursor,
                 server_->daemonSessionCursor(target->threadId, target->daemonGeneration));
    if (target->lastSeenCursor >= target->lastEventCursor)
        return false;
    const qint64 cursor = qMax(target->lastEventCursor, target->lastSeenCursor);
    target->lastSeenCursor = cursor;
    if (!server_->usesDaemon() || target->threadId.isEmpty())
        return true;
    const QString path = target->path;
    server_->markDaemonSessionSeen(
        target->threadId, cursor,
        [this, path, weak = std::weak_ptr<Session>(target)](const QJsonObject& result,
                                                            const QString& error) {
            if (!error.isEmpty())
                return;
            const SessionPtr session = weak.lock();
            if (!session)
                return;
            session->lastSeenCursor =
                qMax(session->lastSeenCursor, result.value(QStringLiteral("last_seen_cursor"))
                                                  .toInteger(session->lastSeenCursor));
            session->lastEventCursor =
                qMax(session->lastEventCursor, result.value(QStringLiteral("last_event_cursor"))
                                                   .toInteger(session->lastEventCursor));
            emit conversationsChanged(path, conversations(path));
        });
    return true;
}

void AgentSessionManager::selectSessionGeneration(const SessionPtr& session) {
    const SessionPtr previous = sessions_.value(activePath_);
    if (session && previous == session && activePath_ == session->path) {
        if (session->selectionGeneration == 0)
            session->selectionGeneration = ++nextSelectionGeneration_;
        return;
    }
    if (previous && previous != session) {
        // Canonical text must not be lost when navigation beats the UI timer.
        flushAgentDeltas(previous);
        // With daemon windows, only a canceled attach or first window leaves
        // nothing valid behind. A canceled older or newer page leaves the loaded
        // window and its cursors as they were (a cursor advances only when its
        // page is applied); discarding that window would reopen the
        // conversation at its newest turns and move the reader to the bottom of
        // a different window.
        const bool keepsWindow = server_->hasDaemonTranscriptWindow() && previous->historyLoaded;
        const bool canceledHistoryRequest =
            previous->attaching || (previous->historyLoading && !keepsWindow);
        if (previous->historyLoading && !canceledHistoryRequest)
            transcript_trace::write(QStringLiteral("history-page-canceled"),
                                    {{"rows", previous->messages.size()}});
        ++previous->historyRequestGeneration;
        // Projection, measurement and idle publication use their own fence.
        // Cancel those too before a rapid switch can publish an obsolete batch.
        ++previous->historyEpoch;
        cancelDaemonHistoryCatchup(previous);
        clearDaemonHistoryCache(*previous);
        previous->attaching = false;
        previous->historyLoading = false;
        previous->historyRefreshingLatest = false;
        if (canceledHistoryRequest)
            previous->historyLoaded = false;
        else
            trimDaemonHistoryWindow(previous, false);
        previous->pendingFinalDeltas.clear();
        previous->scheduledFinalDeltaFlushes.clear();
    }
    if (session)
        session->selectionGeneration = ++nextSelectionGeneration_;
}

void AgentSessionManager::renameConversation(const QString& worktreePath,
                                             const QString& conversationId, const QString& title) {
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || conversationId.isEmpty()) {
        return;
    }
    const SessionPtr current = ensureSession(worktreePath);
    if (!current) {
        return;
    }
    for (const SessionPtr& session : conversationsByPath_.value(current->path)) {
        if (session->id != conversationId) {
            continue;
        }
        session->title = trimmed;
        session->customTitle = true;
        persistWorkspace(session->path);
        syncConversationTitle(session);
        emit conversationsChanged(session->path, conversations(session->path));
        if (isActiveSession(session)) {
            emit sessionStateChanged(session->path, session->title, status(session->path),
                                     session->running);
        }
        return;
    }
}

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

bool AgentSessionManager::cancelConversation(const QString& worktreePath,
                                             const QString& conversationId) {
    const QString path = normalizedPath(worktreePath);
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->id == conversationId && session->running) {
            cancelSession(session);
            return true;
        }
    }
    return false;
}

bool AgentSessionManager::cancelWorktree(const QString& worktreePath) {
    const QString path = normalizedPath(worktreePath);
    bool canceled = false;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (session->running) {
            cancelSession(session);
            canceled = true;
        }
    }
    return canceled;
}

void AgentSessionManager::archiveConversationsForWorktree(const QString& worktreePath) {
    const QString path = normalizedPath(worktreePath);
    (void)ensureSession(path);
    bool changed = false;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        if (!session->archived) {
            session->archived = true;
            changed = true;
        }
    }
    if (changed) {
        persistWorkspace(path);
        emit conversationsChanged(path, conversations(path));
        emit activityChanged();
    }
}

void AgentSessionManager::forgetDeletedConversation(const SessionPtr& target) {
    const QString path = target->path;
    if (!conversationsByPath_.value(path).contains(target))
        return;
    if (worktreeStore_)
        worktreeStore_->removeConversation(target->id);
    auto& current = conversationsByPath_[path];
    current.removeAll(target);
    if (!target->threadId.isEmpty()) {
        sessionsByThread_.remove(target->threadId);
        deletedThreadsByPath_[path].insert(target->threadId);
    }
    if (sessions_.value(path) == target) {
        SessionPtr replacement;
        for (auto it = current.crbegin(); it != current.crend(); ++it) {
            if (!(*it)->archived) {
                replacement = *it;
                break;
            }
        }
        if (!replacement && QFileInfo(path).isDir()) {
            replacement = std::make_shared<Session>();
            replacement->path = path;
            current.append(replacement);
        }
        if (replacement)
            sessions_.insert(path, replacement);
        else
            sessions_.remove(path);
        emit messagesReset(path, messages(path));
        if (replacement) {
            emit sessionStateChanged(path, replacement->title, replacement->status,
                                     replacement->running);
            emitConfiguration(replacement);
            emitHistoryState(replacement);
        }
    }
    persistWorkspace(path);
    emit conversationsChanged(path, conversations(path));
    emit activityChanged();
    emit conversationDeletionFinished(path, target->id, {});
}

bool AgentSessionManager::deleteConversation(const QString& worktreePath,
                                             const QString& conversationId) {
    const QString path = normalizedPath(worktreePath);
    auto& values = conversationsByPath_[path];
    const auto found = std::find_if(values.begin(), values.end(), [&conversationId](const auto& s) {
        return s->id == conversationId;
    });
    if (found == values.end() || (*found)->running) {
        emit conversationDeletionFinished(
            path, conversationId,
            found == values.end() ? QStringLiteral("The conversation could not be found.")
                                  : QStringLiteral("Stop the conversation before deleting it."));
        return false;
    }
    const SessionPtr target = *found;
    const auto finish = [this, target] { forgetDeletedConversation(target); };
    if (target->threadId.isEmpty()) {
        finish();
        return true;
    }
    server_->call(QStringLiteral("thread/delete"), {{QStringLiteral("threadId"), target->threadId}},
                  [this, path, target, finish](const QJsonObject&, const QString& error) {
                      if (!error.isEmpty()) {
                          emit conversationDeletionFinished(path, target->id, error);
                          emit errorOccurred(
                              QStringLiteral("The conversation was not deleted: %1").arg(error));
                          return;
                      }
                      finish();
                  });
    return true;
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
        persistWorkspace(path);
        emit conversationsChanged(path, conversations(path));
        emit activityChanged();
        return true;
    }
    return false;
}

void AgentSessionManager::syncConversationTitle(const SessionPtr& session) {
    if (!session || session->threadId.isEmpty() || !server_->usesDaemon()) {
        return;
    }
    server_->call(
        QStringLiteral("thread/name/set"),
        {{QStringLiteral("threadId"), session->threadId}, {QStringLiteral("name"), session->title}},
        [this](const QJsonObject&, const QString& error) {
            if (!error.isEmpty()) {
                emit errorOccurred(
                    QStringLiteral("The conversation was renamed locally, but the Codex "
                                   "thread name could not be updated: %1")
                        .arg(error));
            }
        });
}

AgentSessionManager::SessionPtr AgentSessionManager::ensureSession(const QString& worktreePath) {
    const QString path = normalizedPath(worktreePath);
    if (path.isEmpty()) {
        return {};
    }
    auto found = sessions_.find(path);
    if (found != sessions_.end()) {
        return found.value();
    }
    const QVariantMap persisted = QSettings().value(persistedKey(path)).toMap();
    const QStringList deletedThreadIds =
        persisted.value(QStringLiteral("deletedThreadIds")).toStringList();
    deletedThreadsByPath_.insert(path,
                                 QSet<QString>(deletedThreadIds.cbegin(), deletedThreadIds.cend()));
    QVector<SessionPtr> restored;
    if (persisted.value(QStringLiteral("path")).toString() == path) {
        for (const QVariant& value : persisted.value(QStringLiteral("conversations")).toList()) {
            const QVariantMap stored = value.toMap();
            auto session = std::make_shared<Session>();
            session->id = stored.value(QStringLiteral("id"), session->id).toString();
            session->path = path;
            session->threadId = stored.value(QStringLiteral("threadId")).toString();
            if (deletedDaemonGenerations_.contains(session->threadId))
                continue;
            session->title = stored.value(QStringLiteral("title"), session->title).toString();
            session->customTitle = stored.value(QStringLiteral("customTitle")).toBool();
            session->archived = stored.value(QStringLiteral("archived")).toBool();
            session->ownsWorktree = stored.value(QStringLiteral("ownsWorktree")).toBool();
            session->listed =
                stored.value(QStringLiteral("listed")).toBool() || session->ownsWorktree;
            session->model = stored.value(QStringLiteral("model")).toString();
            session->effort = stored.value(QStringLiteral("effort")).toString();
            session->permission =
                stored.value(QStringLiteral("permission"), QStringLiteral("workspace-write"))
                    .toString();
            session->options.insert(QStringLiteral("permission"), session->permission);
            const QString provider = stored.value(QStringLiteral("provider")).toString().trimmed();
            if (!provider.isEmpty())
                session->options.insert(QStringLiteral("provider"), provider.toLower());
            if (!session->model.isEmpty()) {
                session->options.insert(QStringLiteral("model"), session->model);
            }
            if (!session->effort.isEmpty()) {
                session->options.insert(QStringLiteral("effort"), session->effort);
            }
            session->contextUsedTokens =
                stored.value(QStringLiteral("contextUsedTokens"), qint64(-1)).toLongLong();
            session->contextLimitTokens =
                stored.value(QStringLiteral("contextLimitTokens")).toLongLong();
            session->planLimits = stored.value(QStringLiteral("planLimits")).toList();
            session->sessionUsage = stored.value(QStringLiteral("sessionUsage")).toMap();
            session->lastTurnStartedAt =
                stored.value(QStringLiteral("lastTurnStartedAt")).toLongLong();
            session->status = session->threadId.isEmpty() ? QStringLiteral("Ready")
                                                          : QStringLiteral("Saved conversation");
            if (!session->threadId.isEmpty()) {
                sessionsByThread_.insert(session->threadId, session);
            }
            restored.append(session);
        }
        // Migrate the original single-conversation settings shape.
        if (restored.isEmpty() && persisted.contains(QStringLiteral("threadId"))) {
            auto session = std::make_shared<Session>();
            session->path = path;
            session->threadId = persisted.value(QStringLiteral("threadId")).toString();
            session->title = persisted.value(QStringLiteral("title"), session->title).toString();
            session->model = persisted.value(QStringLiteral("model")).toString();
            session->effort = persisted.value(QStringLiteral("effort")).toString();
            session->permission =
                persisted.value(QStringLiteral("permission"), QStringLiteral("workspace-write"))
                    .toString();
            session->options.insert(QStringLiteral("permission"), session->permission);
            const QString provider =
                persisted.value(QStringLiteral("provider")).toString().trimmed();
            if (!provider.isEmpty())
                session->options.insert(QStringLiteral("provider"), provider.toLower());
            if (!session->model.isEmpty()) {
                session->options.insert(QStringLiteral("model"), session->model);
            }
            if (!session->effort.isEmpty()) {
                session->options.insert(QStringLiteral("effort"), session->effort);
            }
            session->contextUsedTokens =
                persisted.value(QStringLiteral("contextUsedTokens"), qint64(-1)).toLongLong();
            session->contextLimitTokens =
                persisted.value(QStringLiteral("contextLimitTokens")).toLongLong();
            session->status = session->threadId.isEmpty() ? QStringLiteral("Ready")
                                                          : QStringLiteral("Saved conversation");
            if (!session->threadId.isEmpty()) {
                sessionsByThread_.insert(session->threadId, session);
            }
            restored.append(session);
        }
    }
    if (restored.isEmpty()) {
        if (!QFileInfo(path).isDir()) {
            return {};
        }
        auto session = std::make_shared<Session>();
        session->path = path;
        const QVariantMap last = lastConversationConfiguration();
        session->model = last.value(QStringLiteral("model")).toString();
        session->effort = last.value(QStringLiteral("effort")).toString();
        session->permission =
            last.value(QStringLiteral("permission"), QStringLiteral("workspace-write")).toString();
        session->options.insert(QStringLiteral("permission"), session->permission);
        const QString provider = last.value(QStringLiteral("provider")).toString().trimmed();
        if (!provider.isEmpty())
            session->options.insert(QStringLiteral("provider"), provider.toLower());
        if (!session->model.isEmpty())
            session->options.insert(QStringLiteral("model"), session->model);
        if (!session->effort.isEmpty())
            session->options.insert(QStringLiteral("effort"), session->effort);
        restored.append(session);
    }
    conversationsByPath_.insert(path, restored);
    const QString activeId = persisted.value(QStringLiteral("activeId")).toString();
    SessionPtr active = restored.constLast();
    for (const SessionPtr& session : restored) {
        if (session->id == activeId) {
            active = session;
            break;
        }
    }
    sessions_.insert(path, active);
    return active;
}

AgentSessionManager::SessionPtr
AgentSessionManager::sessionForThread(const QString& threadId) const {
    return sessionsByThread_.value(threadId);
}

void AgentSessionManager::acceptDaemonConfiguration(const SessionPtr& session,
                                                    const QJsonObject& settings) {
    // The daemon's model belongs to the agent it runs the conversation on. A provider kept
    // from an earlier launch, or from before another client moved the conversation, would
    // pair that model with the wrong agent, which then rejects every turn.
    const QString adapter = settings.value("adapter").toString().trimmed().toLower();
    const bool adapterChanged = !adapter.isEmpty() && adapter != session->daemonAdapter;
    if (adapterChanged) {
        session->daemonAdapter = adapter;
        session->options.insert(QStringLiteral("provider"), adapter);
    }
    const auto revision = settings.value("revision").toInteger();
    const auto effective = settings.value("effective").toObject();
    if (revision < session->configurationRevision || revision <= 0 || effective.isEmpty()) {
        if (adapterChanged) {
            persistWorkspace(session->path);
            emitConfiguration(session);
        }
        return;
    }
    session->configurationRevision = revision;
    if (effective.contains("compactWindow"))
        session->options.insert("compactWindow", effective.value("compactWindow").toVariant());
    // Another agent chosen here for the next turn keeps its own model until that turn moves
    // the conversation; the daemon's model is the current agent's.
    const QString provider = storedProvider(session->options);
    if (session->daemonAdapter.isEmpty() || provider.isEmpty() ||
        provider == session->daemonAdapter) {
        session->model = effective.value("model").toString();
        session->effort = effective.value("effort").toString();
    }
    const auto policy = effective.value("execution_policy").toString();
    session->permission = policy == "full"        ? "danger-full-access"
                          : policy == "read-only" ? "read-only"
                                                  : "workspace-write";
    session->options.insert("model", session->model);
    session->options.insert("effort", session->effort);
    session->options.insert("permission", session->permission);
    persistWorkspace(session->path);
    emitConfiguration(session);
}

void AgentSessionManager::handleDaemonSession(const QJsonObject& stored) {
    const QString path = normalizedPath(
        stored.value("conversation_path").toString(stored.value("worktree_path").toString()));
    const QString sessionId = stored.value(QStringLiteral("session_id")).toString();
    if (path.isEmpty() || sessionId.isEmpty()) {
        return;
    }
    if (deletedDaemonGenerations_.contains(sessionId)) {
        if (stored.value("session_generation").toInteger() <=
            deletedDaemonGenerations_.value(sessionId))
            return;
        deletedDaemonGenerations_.remove(sessionId);
        deletedThreadsByPath_[path].remove(sessionId);
    }
    if (deletedThreadsByPath_.value(path).contains(sessionId))
        return;
    const bool wasRelevant =
        conversationsByPath_.contains(path) && !conversationsByPath_.value(path).isEmpty();
    // Session discovery can arrive before the workspace is activated. Restore
    // the local conversation ids and custom titles before matching the daemon
    // session, otherwise the default projection overwrites QSettings.
    if (!conversationsByPath_.contains(path)) {
        (void)ensureSession(path);
    }
    SessionPtr session = sessionForThread(sessionId);
    if (!session) {
        auto& conversations = conversationsByPath_[path];
        if (conversations.size() == 1 && conversations.first()->threadId.isEmpty() &&
            conversations.first()->messages.isEmpty()) {
            session = conversations.first();
            session->id = sessionId;
        } else {
            session = std::make_shared<Session>();
            session->id = sessionId;
            session->path = path;
            conversations.append(session);
        }
        session->threadId = sessionId;
        sessionsByThread_.insert(sessionId, session);
        if (!sessions_.contains(path) || sessions_.value(path)->threadId.isEmpty()) {
            sessions_.insert(path, session);
        }
    }
    if (session->daemonGeneration != stored.value("session_generation").toInteger() ||
        stored.value("context_revision").toInteger(1) >= session->executionRevision) {
        session->executionPath = normalizedPath(stored.value("worktree_path").toString(path));
        session->executionRevision = stored.value("context_revision").toInteger(1);
    }
    if (session->daemonGeneration != stored.value("session_generation").toInteger())
        session->configurationRevision = 0;
    acceptDaemonConfiguration(session, {{"revision", stored.value("configuration_revision")},
                                        {"effective", stored.value("effective")},
                                        {"adapter", stored.value("adapter")}});
    if (const QString number = stored.value(QStringLiteral("number")).toString(); !number.isEmpty())
        session->number = number;
    const QJsonObject agentTurn = stored.value(QStringLiteral("agent_turn")).toObject();
    session->agentTurnNumber = agentTurn.value(QStringLiteral("number")).toString();
    session->agentTurnTitle = agentTurn.value(QStringLiteral("title")).toString();
    session->agentTurnReply =
        agentTurn.value(QStringLiteral("kind")).toString() == QStringLiteral("reply");
    const QString daemonTitle = stored.value(QStringLiteral("title")).toString().trimmed();
    if (!session->customTitle && !daemonTitle.isEmpty()) {
        session->title = daemonTitle;
    } else if (session->customTitle && daemonTitle != session->title) {
        syncConversationTitle(session);
    }
    const bool wasRunning = session->running;
    const auto generation = stored.value("session_generation").toInteger();
    if (session->daemonGeneration != 0 && session->daemonGeneration != generation) {
        cancelDaemonHistoryCatchup(session);
        session->historyLoaded = false;
        session->historyDirty = false;
        session->messages.clear();
        session->messageIndexByItem.clear();
        session->loadedIndexByItem.clear();
        session->pendingQuestionRequests.clear();
        session->pendingInputIds.clear();
        session->activeCompactions.clear();
        session->loadedTurns.clear();
        session->historyHasNewer = false;
        session->historyNewerCursor.clear();
        session->historyTurnCursors.clear();
        session->copying = false;
        ++session->copyGeneration;
        session->copyMessages.clear();
    }
    session->daemonGeneration = generation;
    session->lastEventCursor = qMax(session->lastEventCursor,
                                    stored.value(QStringLiteral("last_event_cursor")).toInteger());
    session->lastSeenCursor =
        qMax(session->lastSeenCursor, stored.value(QStringLiteral("last_seen_cursor")).toInteger());
    // The daemon's time replaces one this desktop recorded, so phones see the same order.
    if (const qint64 startedAt =
            daemonTimestamp(stored.value(QStringLiteral("last_turn_started_at")).toString());
        startedAt > 0)
        session->lastTurnStartedAt = startedAt;
    const QString state = stored.value(QStringLiteral("state")).toString();
    const auto display = daemonDisplayState(state);
    session->running = display.running;
    session->status = display.status;
    if (!session->running &&
        (state == QStringLiteral("ready") || state == QStringLiteral("completed") ||
         state == QStringLiteral("interrupted")))
        session->completed = session->lastEventCursor > 0;
    session->readyInProcess = true;
    session->historyLoading = server_->isDaemonSessionRestoring(sessionId);
    if (server_->hasDaemonTranscriptWindow()) {
        cancelDaemonHistoryCatchup(session);
        ++session->historyEpoch;
        session->historyDirty = session->historyLoaded;
        session->historyLoading = false;
    } else {
        session->historyLoaded = !session->historyLoading;
    }
    persistWorkspace(path);
    emit conversationsChanged(path, conversations(path));
    if (isActiveSession(session)) {
        if (path == activePath_) {
            updateRestoreState(session);
        }
        emit sessionStateChanged(path, session->title, session->status, session->running);
        emitHistoryState(session);
        if (path == activePath_ && server_->hasDaemonTranscriptWindow()) {
            server_->prioritizeDaemonRestore(path, sessionId);
            hydrateConversation(session);
        }
    }
    if (!wasRelevant || wasRunning != session->running) {
        emit activityChanged();
    }
}

void AgentSessionManager::updateRestoreState(const SessionPtr& session) {
    // Paging an already visible conversation must not reset its reading UI or close previews.
    const bool restoring = session && !session->threadId.isEmpty() && session->historyLoading &&
                           !session->historyLoaded && !session->historyRefreshingLatest;
    if (restoring_ == restoring) {
        return;
    }
    restoring_ = restoring;
    emit restoreStateChanged(restoring_);
}

} // namespace ketplus
