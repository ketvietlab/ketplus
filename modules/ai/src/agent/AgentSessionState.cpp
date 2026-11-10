#include "agent/AgentSessionManager.h"

#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/ProviderRuntimeStatus.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionState.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptNormalizer.h"
#include "agent/TranscriptTrace.h"

namespace ketplus {
using namespace agent_message;

int AgentSessionManager::messageIndexForItem(const SessionPtr& session, const QString& itemId) {
    int index = session->messageIndexByItem.value(itemId, -1);
    if (index >= 0 || itemId.isEmpty() || !server_->hasDaemonTranscriptWindow())
        return index;
    // messageIndexByItem only spans the current turn scope. A loaded window row from an
    // earlier turn is found through loadedIndexByItem, which covers every turn, so a late
    // update edits it in place whether or not the event carried that turn's scope.
    const int candidate = session->loadedIndexByItem.value(itemId, -1);
    if (candidate < 0 || candidate >= session->messages.size() ||
        session->messages.at(candidate).value(QStringLiteral("itemId")).toString() != itemId)
        return -1;
    session->messageIndexByItem.insert(itemId, candidate);
    return candidate;
}

void AgentSessionManager::emitConfiguration(const SessionPtr& session) {
    if (isActiveSession(session)) {
        if (!session->executionPath.isEmpty())
            emit executionContextChanged(session->path, session->id,
                                         session->executionBranch.isEmpty()
                                             ? QFileInfo(session->executionPath).fileName()
                                             : session->executionBranch,
                                         session->executionPath, session->executionRevision);
        emit conversationConfigurationChanged(session->path,
                                              conversationConfiguration(session->path));
    }
}

void AgentSessionManager::emitUsageSnapshot(const SessionPtr& session) {
    if (isActiveSession(session))
        emit usageSnapshotChanged(session->path, usageSnapshot(session->path));
}

void AgentSessionManager::emitBackgroundTasks(const SessionPtr& session) {
    if (isActiveSession(session))
        emit backgroundTasksChanged(session->path, session->backgroundTasks);
}

int AgentSessionManager::appendMessage(const SessionPtr& session, QVariantMap entry) {
    const int index = session->messages.size();
    if (entry.value(QStringLiteral("messageKey")).toString().isEmpty())
        entry.insert("messageKey", QUuid::createUuid().toString(QUuid::WithoutBraces));
    entry.insert("turnId", session->turnId);
    if (server_->hasDaemonTranscriptWindow() && !entry.value("itemId").toString().isEmpty()) {
        entry.insert("messageKey", daemonMessageKey(session->daemonTurnSequence,
                                                    entry.value("itemId").toString()));
        entry.insert("turnId", daemonTurnId(session->daemonTurnSequence));
    }
    labelProvider(entry, session->options.value(QStringLiteral("provider")).toString());
    session->messages.append(std::move(entry));
    if (server_->hasDaemonTranscriptWindow() && session->historyLoading &&
        !session->historyLoaded && session->daemonLiveEventCursor > 0)
        session->daemonLiveRowCursors.insert(
            session->messages.constLast().value("messageKey").toString(),
            session->daemonLiveEventCursor);
    if (isActiveSession(session)) {
        emit messageAppended(session->path, index, session->messages.constLast());
    }
    // The loaded window ends before the newest turns, so this live row lands
    // after a gap, and newer pages would later be merged in after it. Replace
    // the window with the latest one, which carries this row in its place.
    if (session->historyHasNewer && server_->hasDaemonTranscriptWindow() &&
        !session->historyLoading) {
        transcript_trace::write(QStringLiteral("live-append-behind-window"),
                                {{QStringLiteral("index"), index},
                                 {QStringLiteral("active"), isActiveSession(session)}});
        loadLatestWindow(session);
    }
    return index;
}

void AgentSessionManager::updateMessage(const SessionPtr& session, const int index,
                                        QVariantMap entry) {
    if (index < 0 || index >= session->messages.size()) {
        return;
    }
    entry.insert("messageKey", session->messages.at(index).value("messageKey"));
    entry.insert("turnId", session->messages.at(index).value("turnId"));
    labelProvider(entry, session->options.value(QStringLiteral("provider")).toString());
    session->messages[index] = std::move(entry);
    if (server_->hasDaemonTranscriptWindow() && session->historyLoading &&
        !session->historyLoaded && session->daemonLiveEventCursor > 0)
        session->daemonLiveRowCursors.insert(
            session->messages.at(index).value("messageKey").toString(),
            session->daemonLiveEventCursor);
    if (isActiveSession(session)) {
        emit messageUpdated(session->path, index, session->messages.at(index));
    }
}

void AgentSessionManager::appendAgentDelta(const SessionPtr& session, const QString& itemId,
                                           const QString& delta) {
    if (delta.isEmpty()) {
        return;
    }
    const QString phase = session->agentMessagePhaseByItem.value(itemId);
    int index = messageIndexForItem(session, itemId);
    // A bounded daemon window already projected public assistant rows, but did
    // not replay their item/started notifications into the live phase map.
    // Continue those rows without waiting for another start or a final snapshot.
    // Unclassified new items still wait for an explicitly public phase.
    const bool restoredAssistant =
        phase.isEmpty() && server_->hasDaemonTranscriptWindow() && index >= 0 &&
        index < session->messages.size() &&
        session->messages.at(index).value(QStringLiteral("kind")) == QStringLiteral("assistant");
    if (phase != QStringLiteral("final_answer") && phase != QStringLiteral("commentary") &&
        !restoredAssistant) {
        if (!session->deferredAgentOrder.contains(itemId))
            session->deferredAgentOrder.append(itemId);
        session->deferredAgentTextByItem[itemId].append(delta);
        if (server_->hasDaemonTranscriptWindow() && session->historyLoading &&
            !session->historyLoaded && session->daemonLiveEventCursor > 0)
            session->daemonDeferredDeltasByItem[itemId].append(
                {session->daemonLiveEventCursor, delta});
        return;
    }
    if (index < 0) {
        const QJsonObject item{{QStringLiteral("id"), itemId},
                               {QStringLiteral("type"), QStringLiteral("agentMessage")},
                               {QStringLiteral("phase"), phase},
                               {QStringLiteral("text"), QString{}}};
        const auto event =
            ActivityNormalizer::normalize(item, TranscriptPhase::Started,
                                          {session->id, session->transcriptGeneration,
                                           session->turnId, session->messages.size(), 1});
        index = appendMessage(session, event.toVariantMap());
        session->messageIndexByItem.insert(itemId, index);
    }
    session->pendingFinalDeltas[itemId].append(delta);
    if (session->scheduledFinalDeltaFlushes.contains(itemId))
        return;
    session->scheduledFinalDeltaFlushes.insert(itemId);
    const quint64 selectionGeneration = session->selectionGeneration;
    const QPointer<AgentSessionManager> guard(this);
    QTimer::singleShot(16, this, [guard, this, session, itemId, selectionGeneration] {
        if (guard)
            flushAgentDelta(session, itemId, selectionGeneration);
    });
}

void AgentSessionManager::flushAgentDelta(const SessionPtr& session, const QString& itemId,
                                          const quint64 selectionGeneration) {
    if (selectionGeneration != 0 && session->selectionGeneration != selectionGeneration)
        return;
    session->scheduledFinalDeltaFlushes.remove(itemId);
    const QString delta = session->pendingFinalDeltas.take(itemId);
    const int index = session->messageIndexByItem.value(itemId, -1);
    if (delta.isEmpty() || index < 0 || index >= session->messages.size())
        return;
    QVariantMap entry = session->messages.at(index);
    entry.insert(QStringLiteral("body"), entry.value(QStringLiteral("body")).toString() + delta);
    updateMessage(session, index, entry);
}

void AgentSessionManager::flushAgentDeltas(const SessionPtr& session) {
    const QStringList itemIds = session->pendingFinalDeltas.keys();
    for (const QString& itemId : itemIds)
        flushAgentDelta(session, itemId);
}

void AgentSessionManager::publishDeferredFinal(const SessionPtr& session) {
    if (!session || session->explicitFinalInTurn)
        return;
    for (auto it = session->deferredAgentOrder.crbegin(); it != session->deferredAgentOrder.crend();
         ++it) {
        const QString text = session->deferredAgentTextByItem.value(*it).trimmed();
        if (text.isEmpty())
            continue;
        const QJsonObject item{{QStringLiteral("id"), *it},
                               {QStringLiteral("type"), QStringLiteral("agentMessage")},
                               {QStringLiteral("phase"), QStringLiteral("final_answer")},
                               {QStringLiteral("text"), text}};
        const auto event =
            ActivityNormalizer::normalize(item, TranscriptPhase::Completed,
                                          {session->id, session->transcriptGeneration,
                                           session->turnId, session->messages.size(), 1});
        const auto entry = event.toVariantMap();
        const int index = appendMessage(session, entry);
        session->messageIndexByItem.insert(*it, index);
        session->explicitFinalInTurn = true;
        break;
    }
}

void AgentSessionManager::dismissPendingQuestions(const SessionPtr& session) {
    // Only the daemon's resolution/expiry events may close its pending cards.
    // A stale metadata snapshot or an inactive UI is not a user decision.
    if (server_->usesDaemon())
        return;
    for (int index = 0; index < session->messages.size(); ++index) {
        QVariantMap entry = session->messages.at(index);
        const auto key = entry.value("messageKey").toString();
        if (!session->pendingQuestionRequests.contains(key))
            continue;
        const auto requestId = session->pendingQuestionRequests.take(key);
        if (!server_->usesDaemon())
            server_->respondToUserInput(requestId, {});
        entry.insert("running", false);
        entry.insert("meta", "Agent · question ended");
        updateMessage(session, index, entry);
    }
}

void AgentSessionManager::setWorkingDetail(const SessionPtr& session, const QString& detail) {
    if (session->workingDetail == detail)
        return;
    session->workingDetail = detail;
    // A background conversation in the same worktree keeps its own detail; the line shows the
    // active one, which a conversation switch reads again through workingDetail().
    if (isActiveSession(session))
        emit workingDetailChanged(session->path, detail);
}

QString AgentSessionManager::workingDetail(const QString& worktreePath) const {
    const auto found = sessions_.constFind(normalizedPath(worktreePath));
    return found != sessions_.cend() && (*found)->running ? (*found)->workingDetail : QString();
}

void AgentSessionManager::updateState(const SessionPtr& session, const QString& state,
                                      const bool running) {
    ++session->stateSerial;
    const bool activityChanged = session->running != running;
    if (running || state == QStringLiteral("Error") || state == QStringLiteral("Stopped") ||
        state == QStringLiteral("Canceled"))
        session->completed = false;
    if (!running) {
        setWorkingDetail(session, {});
        session->pendingInputIds.clear();
        settleActivityGroups(session);
        dismissPendingQuestions(session);
        session->pendingCompact = false;
        for (const int index : std::as_const(session->messageIndexByItem)) {
            if (index < 0 || index >= session->messages.size())
                continue;
            auto entry = session->messages.at(index);
            if (session->pendingQuestionRequests.contains(entry.value("messageKey").toString()))
                continue;
            if (entry.value("category").toString() != QStringLiteral("activity") ||
                !entry.value("running").toBool())
                continue;
            entry.insert("running", false);
            entry.insert("phase", QStringLiteral("interrupted"));
            entry.insert("status", QStringLiteral("interrupted"));
            if (entry.value("subtype") == QStringLiteral("webSearch"))
                entry.insert("meta", QStringLiteral("Search web · stopped"));
            updateMessage(session, index, entry);
        }
        for (const auto& itemId : session->activeCompactions) {
            const int index = session->messageIndexByItem.value(itemId, -1);
            if (index < 0)
                continue;
            if (index < 0 || index >= session->messages.size())
                continue;
            auto entry = session->messages.at(index);
            entry.insert(QStringLiteral("body"),
                         QStringLiteral("Compaction ended without completion confirmation"));
            entry.insert(QStringLiteral("running"), false);
            updateMessage(session, index, entry);
        }
        session->activeCompactions.clear();
    }
    session->status = running && !session->cancelRequested && !session->activeCompactions.isEmpty()
                          ? QStringLiteral("Compacting context…")
                          : state;
    if (running && !session->running) {
        auto& ordered = conversationsByPath_[session->path];
        ordered.removeAll(session);
        ordered.prepend(session);
        // A daemon reports the start time with its user.message (daemon/turnStartedAt).
        if (!server_->usesDaemon())
            session->lastTurnStartedAt = QDateTime::currentMSecsSinceEpoch();
        persistWorkspace(session->path);
    }
    session->running = running;
    if (isActiveSession(session)) {
        emit sessionStateChanged(session->path, session->title, session->status, running);
    }
    emit conversationsChanged(session->path, conversations(session->path));
    if (activityChanged) {
        emit this->activityChanged();
    }
}

void AgentSessionManager::recordSessionError(const SessionPtr& session, const QString& error,
                                             const bool notify, const QString& code) {
    const QString detail = error.isEmpty() ? QStringLiteral("Unknown Codex error") : error;
    auto entry = message(QStringLiteral("error"), detail, QStringLiteral("Error"));
    entry.insert(QStringLiteral("summary"), detail);
    agent_message::presentAgentError(entry, detail);
    if (!code.isEmpty()) {
        // The row shows the provider's setup guide; the readiness it changed is read again.
        entry.insert(QStringLiteral("errorCode"), code);
        entry.insert(QStringLiteral("body"), detail); // the guide quotes the provider's words
        entry.insert(QStringLiteral("provider"), session->daemonAdapter);
        providerStatus_->refresh();
    }
    appendMessage(session, entry);
    if (notify)
        emit errorOccurred(detail);
}

void AgentSessionManager::failSession(const SessionPtr& session, const QString& error,
                                      const QString& code) {
    recordSessionError(session, error, false, code);
    session->pendingPrompt.clear();
    session->turnId.clear();
    session->cancelRequested = false;
    updateState(session, QStringLiteral("Error"), false);
    emit errorOccurred(error.isEmpty() ? QStringLiteral("Unknown Codex error") : error);
}

void AgentSessionManager::persistWorkspace(const QString& path) const {
    const SessionPtr active = sessions_.value(path);
    QVariantList stored;
    for (const SessionPtr& session : conversationsByPath_.value(path)) {
        stored.append(
            QVariantMap{{QStringLiteral("id"), session->id},
                        {QStringLiteral("threadId"), session->threadId},
                        {QStringLiteral("title"), session->title},
                        {QStringLiteral("customTitle"), session->customTitle},
                        {QStringLiteral("archived"), session->archived},
                        {QStringLiteral("archiveUnsent"), session->archiveUnsent},
                        {QStringLiteral("ownsWorktree"), session->ownsWorktree},
                        {QStringLiteral("listed"), session->listed || session->ownsWorktree},
                        {QStringLiteral("model"), session->model},
                        {QStringLiteral("effort"), session->effort},
                        {QStringLiteral("permission"), session->permission},
                        {QStringLiteral("contextUsedTokens"), session->contextUsedTokens},
                        {QStringLiteral("contextLimitTokens"), session->contextLimitTokens},
                        {QStringLiteral("planLimits"), session->planLimits},
                        {QStringLiteral("sessionUsage"), session->sessionUsage},
                        {QStringLiteral("lastTurnStartedAt"), session->lastTurnStartedAt},
                        {QStringLiteral("provider"),
                         session->options.value(QStringLiteral("provider")).toString()}});
    }
    QSettings().setValue(persistedKey(path),
                         QVariantMap{{QStringLiteral("path"), path},
                                     {QStringLiteral("activeId"), active ? active->id : QString{}},
                                     {QStringLiteral("deletedThreadIds"),
                                      QStringList(deletedThreadsByPath_.value(path).values())},
                                     {QStringLiteral("conversations"), stored}});
}

bool AgentSessionManager::isActiveSession(const SessionPtr& session) const {
    return session && sessions_.value(session->path) == session;
}

void AgentSessionManager::applyModelOverride(QJsonObject& params, const bool includeEffort) const {
    if (!modelOverride_.isEmpty()) {
        params.insert(QStringLiteral("model"), modelOverride_);
    }
    if (includeEffort && !effortOverride_.isEmpty()) {
        params.insert(QStringLiteral("effort"), effortOverride_);
    }
}

QString AgentSessionManager::normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

} // namespace ketplus
