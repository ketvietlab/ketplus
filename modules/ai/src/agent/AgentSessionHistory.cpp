#include "agent/AgentSessionManager.h"

#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <QUuid>

#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionState.h"

namespace ketplus {
using namespace agent_message;

void AgentSessionManager::hydrateConversation(const SessionPtr& session, bool loadOlder) {
    if (session && server_->hasDaemonTranscriptWindow()) {
        if (session->readyInProcess) {
            if (session->historyDirty && session->historyLoaded)
                loadLatestWindow(session, true);
            else if (!session->historyLoaded || loadOlder)
                loadDaemonHistoryPage(session);
        }
        return;
    }
    if (!session || session->threadId.isEmpty() || session->attaching || session->running ||
        !isAvailable()) {
        return;
    }
    if (server_->usesDaemon() && !session->readyInProcess) {
        return;
    }
    if (session->readyInProcess) {
        if (!session->historyLoaded || loadOlder)
            loadHistoryPage(session);
        return;
    }
    session->attaching = true;
    const quint64 selectionGeneration = session->selectionGeneration;
    const quint64 requestGeneration = ++session->historyRequestGeneration;
    session->historyError.clear();
    emitHistoryState(session);
    const QString previousStatus = session->status;
    updateState(session, QStringLiteral("Loading conversation…"), false);
    QJsonObject params{{QStringLiteral("threadId"), session->threadId},
                       {QStringLiteral("cwd"), session->path},
                       {QStringLiteral("sandbox"), QStringLiteral("workspace-write")},
                       {QStringLiteral("approvalPolicy"), QStringLiteral("never")},
                       {QStringLiteral("approvalsReviewer"), QStringLiteral("user")},
                       {QStringLiteral("excludeTurns"), true}};
    const auto permissions = CodexAppServer::permissionParams(session->permission, false);
    for (auto it = permissions.begin(); it != permissions.end(); ++it) {
        params.insert(it.key(), it.value());
    }
    if (!session->model.isEmpty()) {
        params.insert(QStringLiteral("model"), session->model);
    }
    const QPointer<AgentSessionManager> guard(this);
    server_->call(
        QStringLiteral("thread/resume"), params,
        [guard, this, session, previousStatus, loadOlder, selectionGeneration,
         requestGeneration](const QJsonObject& result, const QString& error) {
            if (!guard)
                return;
            if (session->selectionGeneration != selectionGeneration ||
                session->historyRequestGeneration != requestGeneration || !isActiveSession(session))
                return;
            session->attaching = false;
            if (!error.isEmpty()) {
                session->historyError = error;
                session->historyErrorNewer = false;
                qWarning().noquote() << "KetPlus: conversation attach failed:" << error;
                emitHistoryState(session);
                emit errorOccurred(QStringLiteral("Unable to load conversation: %1").arg(error));
                updateState(session,
                            previousStatus.isEmpty() ? QStringLiteral("Saved conversation")
                                                     : previousStatus,
                            false);
                return;
            }
            session->readyInProcess = true;
            sessionsByThread_.insert(session->threadId, session);
            if (!session->historyLoaded) {
                const auto thread = result.value("thread").toObject();
                if (!thread.value("turns").toArray().isEmpty())
                    hydrateHistory(session, thread);
                else
                    loadHistoryPage(session);
            } else if (loadOlder)
                loadHistoryPage(session);
            else
                emitHistoryState(session);
            if (!session->running)
                updateState(session,
                            session->messages.isEmpty() ? QStringLiteral("Saved conversation")
                                                        : QStringLiteral("Ready"),
                            false);
        });
}

QVariantMap AgentSessionManager::historyState(const QString& worktreePath) const {
    const auto session = sessions_.value(normalizedPath(worktreePath));
    if (!session)
        return {};
    // Where the loaded turns sit among all of the conversation's events, so the
    // view can show a position in the whole conversation rather than in the
    // few hundred rows it has loaded.
    QVariantMap turnCursors;
    qint64 headCursor = session->historySnapshotCursor;
    for (auto it = session->historyTurnCursors.cbegin(); it != session->historyTurnCursors.cend();
         ++it) {
        turnCursors.insert(it.key(), QVariantList{it->first, it->second});
        headCursor = std::max(headCursor, it->second);
    }
    return {
        {"hasOlder", !session->historyCursor.isEmpty()},
        {"errorNewer", session->historyErrorNewer},
        {"turnCursors", turnCursors},
        {"headCursor", headCursor},
        {"hasNewer", session->historyHasNewer},
        {"canLoadNewer", session->historyHasNewer && !session->historyNewerCursor.isEmpty() &&
                             server_->hasDaemonTranscriptAfterCursor()},
        {"resyncRequired", session->historyResyncRequired},
        {"loading", session->attaching || session->historyLoading || session->historyCatchingUp},
        {"error", session->historyError},
        {"copying", session->copying},
        {"requestToken", session->historyRequestToken},
        {"lastBatch", session->historyLastBatch}};
}

void AgentSessionManager::emitHistoryState(const SessionPtr& session) {
    if (isActiveSession(session))
        emit historyStateChanged(session->path, historyState(session->path));
}

void AgentSessionManager::loadOlderMessages(const QString& worktreePath,
                                            const double viewportHeight,
                                            const double averageRowHeight,
                                            const qint64 requestToken) {
    const auto session = ensureSession(worktreePath);
    if (!session)
        return;
    if (requestToken > 0)
        session->historyRequestToken = requestToken;
    if (session->threadId.isEmpty() || session->attaching) {
        // Declined: publish the token so the view completes this request (while attaching,
        // the request completes when loading ends).
        emitHistoryState(session);
        return;
    }
    if (viewportHeight > 0 && averageRowHeight > 0) {
        session->historyViewportHeight = viewportHeight;
        session->historyAverageRowHeight = averageRowHeight;
    }
    if (session->historyResyncRequired) {
        session->historyLoaded = false;
        session->historyResyncRequired = false;
    }
    if (!session->readyInProcess)
        hydrateConversation(session, true);
    else
        loadHistoryPage(session);
    if (!session->historyLoading)
        emitHistoryState(session); // Declined without starting a page.
}

void AgentSessionManager::loadNewerMessages(const QString& worktreePath,
                                            const double viewportHeight,
                                            const double averageRowHeight,
                                            const qint64 requestToken) {
    const auto session = ensureSession(worktreePath);
    if (!session)
        return;
    if (requestToken > 0)
        session->historyRequestToken = requestToken;
    if (session->threadId.isEmpty() || session->attaching ||
        !server_->hasDaemonTranscriptAfterCursor()) {
        emitHistoryState(session);
        return;
    }
    if (viewportHeight > 0 && averageRowHeight > 0) {
        session->historyViewportHeight = viewportHeight;
        session->historyAverageRowHeight = averageRowHeight;
    }
    const bool wasLoading = session->historyLoading;
    loadDaemonHistoryPage(session, true);
    if (wasLoading || !session->historyLoading)
        emitHistoryState(session); // Declined without starting a page.
}

void AgentSessionManager::loadLatestMessages(const QString& worktreePath) {
    loadLatestWindow(ensureSession(worktreePath));
}

void AgentSessionManager::reopenConversation(const SessionPtr& session) {
    // A window with nothing newer trimmed off and no event since it was left is
    // what the reader last saw. Refreshing it would swap those rows for the newest
    // turn and then backfill the rest: two transcript rebuilds and a lost reading
    // position for no new content.
    const bool current = session->historyLoaded && !session->historyDirty &&
                         !session->historyHasNewer && !session->historyResyncRequired &&
                         server_->isDaemonTranscriptWindowCurrent(session->threadId);
    if (session->readyInProcess && session->historyLoaded && !current &&
        server_->hasDaemonTranscriptWindow())
        loadLatestWindow(session, true);
    else
        hydrateConversation(session);
}

void AgentSessionManager::loadLatestWindow(const SessionPtr& session, const bool force) {
    if (!session || (!force && !session->historyHasNewer) ||
        (!force && (session->historyLoading || session->historyCatchingUp)) ||
        session->threadId.isEmpty() || !server_->hasDaemonTranscriptWindow())
        return;
    // Read a fresh initial window from the durable projection. The historical
    // window remains visible until the fully validated response replaces it.
    session->historyRefreshingLatest = force && session->historyLoaded;
    if (force) {
        cancelDaemonHistoryCatchup(session);
        ++session->historyEpoch;
        session->historyLoading = false;
        clearDaemonHistoryCache(*session);
    }
    session->historyLoaded = false;
    session->historyDirty = false;
    session->historyLatestBackfillPending = false;
    session->historyReadingKey.clear();
    session->historyCursor.clear();
    session->historyNewerCursor.clear();
    session->historySnapshotCursor = 0;
    session->historyProjectionRevision = 0;
    session->historyResyncRequired = false;
    loadDaemonHistoryPage(session);
}

void AgentSessionManager::readHistoryPage(
    const SessionPtr& session, const QString& cursor,
    std::function<void(QJsonArray, QString, QString)> callback) {
    QJsonObject params{{"threadId", session->threadId},
                       {"limit", 20},
                       {"sortDirection", "desc"},
                       {"itemsView", "full"}};
    if (!cursor.isEmpty())
        params.insert("cursor", cursor);
    const QPointer<AgentSessionManager> guard(this);
    server_->call(
        "thread/turns/list", params,
        [guard, this, session, callback = std::move(callback)](const QJsonObject& result,
                                                               const QString& error) {
            if (!guard)
                return;
            if (error.contains("method not found", Qt::CaseInsensitive) ||
                error.contains("-32601") || error.contains("unknown method", Qt::CaseInsensitive)) {
                // Older Codex servers can only return a complete transcript.
                server_->call("thread/read",
                              {{"threadId", session->threadId}, {"includeTurns", true}},
                              [guard, callback](const QJsonObject& full, const QString& failure) {
                                  if (!guard)
                                      return;
                                  callback(full.value("thread").toObject().value("turns").toArray(),
                                           {}, failure);
                              });
                return;
            }
            if (error.isEmpty() && !result.value("data").isArray()) {
                callback({}, {}, QStringLiteral("Codex returned an invalid history page."));
                return;
            }
            QJsonArray chronological;
            const auto data = result.value("data").toArray();
            for (auto it = data.constEnd(); it != data.constBegin();)
                chronological.append(*--it);
            callback(chronological, result.value("nextCursor").toString(), error);
        });
}

void AgentSessionManager::loadHistoryPage(const SessionPtr& session) {
    if (server_->hasDaemonTranscriptWindow()) {
        loadDaemonHistoryPage(session);
        return;
    }
    if (session->historyLoading || (session->historyLoaded && session->historyCursor.isEmpty()))
        return;
    session->historyLoading = true;
    session->historyError.clear();
    emitHistoryState(session);
    const auto threadId = session->threadId;
    const auto cursor = session->historyCursor;
    const quint64 selectionGeneration = session->selectionGeneration;
    const quint64 requestGeneration = ++session->historyRequestGeneration;
    readHistoryPage(
        session, cursor,
        [this, session, threadId, cursor, selectionGeneration,
         requestGeneration](QJsonArray turns, QString next, QString error) {
            if (session->threadId != threadId ||
                session->selectionGeneration != selectionGeneration ||
                session->historyRequestGeneration != requestGeneration || !isActiveSession(session))
                return;
            session->historyLoading = false;
            if (error.isEmpty() && !next.isEmpty() && next == cursor)
                error = QStringLiteral("History cursor did not advance. Please retry.");
            if (!error.isEmpty()) {
                session->historyError = error;
                session->historyErrorNewer = false;
                qWarning().noquote() << "KetPlus: transcript history page failed (older):" << error;
                emitHistoryState(session);
                return;
            }
            session->historyCursor = next;
            hydrateHistory(session, {{"turns", turns}});
        });
}

void AgentSessionManager::copyConversation(const QString& worktreePath) {
    const auto session = ensureSession(worktreePath);
    if (!session || session->copying || session->running)
        return;
    if (!session->threadId.isEmpty() && !session->historyLoaded) {
        emit errorOccurred(QStringLiteral("Load conversation history before copying."));
        return;
    }
    session->copying = true;
    ++session->copyGeneration;
    session->copyMessages = messages(session->path);
    session->copyTurns = session->loadedTurns;
    session->copyCursor = session->historyCursor;
    session->copyCursors.clear();
    if (server_->hasDaemonActivityGroups()) {
        session->copyMessages.clear();
        session->copyCursor = "0";
    }
    emitHistoryState(session);
    copyHistoryPage(session, session->copyGeneration);
}

void AgentSessionManager::copyHistoryPage(const SessionPtr& session, int generation) {
    if (!session->copying || session->copyGeneration != generation)
        return;
    if (server_->hasDaemonTranscriptWindow() && !session->copyCursor.isEmpty()) {
        copyDaemonHistoryPage(session, generation);
        return;
    }
    if (session->copyCursor.isEmpty()) {
        const auto text = transcriptText(session->copyMessages);
        session->copying = false;
        session->copyMessages.clear();
        session->copyTurns.clear();
        emitHistoryState(session);
        emit conversationCopyReady(session->path, session->id, text);
        return;
    }
    const auto cursor = session->copyCursor;
    session->copyCursors.insert(cursor);
    readHistoryPage(
        session, cursor,
        [this, session, generation](QJsonArray turns, QString next, QString error) {
            if (!session->copying || generation != session->copyGeneration)
                return;
            if (error.isEmpty() && !next.isEmpty() && session->copyCursors.contains(next))
                error = QStringLiteral("History cursor did not advance.");
            if (!error.isEmpty()) {
                session->copying = false;
                session->copyMessages.clear();
                session->copyTurns.clear();
                emitHistoryState(session);
                emit errorOccurred(QStringLiteral("Could not copy conversation: %1").arg(error));
                return;
            }
            session->copyMessages = historyMessages(turns, session->copyTurns, session->id,
                                                    session->transcriptGeneration) +
                                    session->copyMessages;
            session->copyCursor = next;
            copyHistoryPage(session, generation);
        });
}

void AgentSessionManager::cancelConversationCopy(const QString& worktreePath) {
    const auto session = sessions_.value(normalizedPath(worktreePath));
    if (!session)
        return;
    ++session->copyGeneration;
    session->copying = false;
    session->copyMessages.clear();
    session->copyTurns.clear();
    emitHistoryState(session);
}

} // namespace ketplus
