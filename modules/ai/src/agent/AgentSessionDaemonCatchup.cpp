#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"
#include "agent/TranscriptTrace.h"

#include <QTimer>

namespace ketplus {

void AgentSessionManager::catchUpDaemonHistory(const SessionPtr& session) {
    if (!session || session->historyLoading || session->historyCatchingUp)
        return;
    clearDaemonHistoryCache(*session);
    const auto fence = session->historyLoaded && !session->messages.isEmpty()
                           ? server_->beginDaemonTranscriptCatchup(session->threadId)
                           : QJsonObject{};
    const auto after = session->historyHasNewer ? session->historyNewerCursor.toLongLong()
                                                : fence.value("after_cursor").toInteger();
    const auto snapshot = fence.value("snapshot_cursor").toInteger();
    if (fence.isEmpty() || after <= 0 || snapshot < after) {
        if (!fence.isEmpty())
            server_->cancelDaemonTranscriptCatchup(session->threadId);
        session->historyDirty = false;
        session->historyLoaded = false;
        loadDaemonHistoryPage(session);
        return;
    }
    session->historyCatchingUp = true;
    session->historyHasNewer = true;
    session->historyNewerCursor = QString::number(after);
    session->historySnapshotCursor = snapshot;
    session->historySnapshotRetries = 0;
    transcript_trace::write(
        QStringLiteral("history-catchup"),
        {{"rows", session->messages.size()}, {"after", after}, {"snapshot", snapshot}});
    // Completion can replace earlier deltas and advance the projection revision
    // while this thread is inactive. Read its current fence without releasing
    // buffered live events or publishing the newest page over the reader.
    refenceDaemonHistory(session, true);
    emitHistoryState(session);
}

void AgentSessionManager::cancelDaemonHistoryCatchup(const SessionPtr& session) {
    if (!session->historyCatchingUp)
        return;
    server_->cancelDaemonTranscriptCatchup(session->threadId);
    session->historyCatchingUp = false;
    session->historyDirty = true;
    session->historyLoading = false;
    ++session->historyEpoch;
}

void AgentSessionManager::failDaemonHistoryCatchup(const SessionPtr& session,
                                                   const QJsonObject& page, const QString& error) {
    if (page.value("error_code") == "snapshot_changed" && session->historySnapshotRetries < 3) {
        ++session->historyEpoch;
        ++session->historySnapshotRetries;
        clearDaemonHistoryCache(*session);
        refenceDaemonHistory(session, true);
        return;
    }
    cancelDaemonHistoryCatchup(session);
    if (page.value("error_code") == "snapshot_changed") {
        // A repeatedly changing fence cannot be joined reliably. Replace only
        // after a fresh page succeeds.
        session->historyDirty = false;
        session->historyLoaded = false;
        clearDaemonHistoryCache(*session);
        loadDaemonHistoryPage(session);
        return;
    }
    session->historyError = error;
    session->historyErrorNewer = true;
    emitHistoryState(session);
    if (isActiveSession(session) && session->path == activePath_)
        updateRestoreState(session);
}

void AgentSessionManager::continueDaemonHistoryCatchup(const SessionPtr& session,
                                                       const QJsonObject& page) {
    if (session->historyHasNewer) {
        const auto epoch = session->historyEpoch;
        QTimer::singleShot(0, this, [this, session, epoch] {
            if (session->historyCatchingUp && session->historyEpoch == epoch)
                loadDaemonHistoryPage(session, true);
        });
        return;
    }
    server_->finishDaemonTranscriptCatchup(
        session->threadId, page, [this, session](const QJsonObject&, const QString& error) {
            if (!error.isEmpty()) {
                // The installed pages remain valid even if live buffering
                // overflowed. Retry from their end, never concatenate them twice.
                session->historyHasNewer = true;
                session->historyNewerCursor = QString::number(session->historySnapshotCursor);
                failDaemonHistoryCatchup(session, {}, error);
                return;
            }
            session->historyCatchingUp = false;
            session->historyDirty = false;
            emitHistoryState(session);
            if (!transcriptScrolling_)
                historyTrimSettle_.start();
            scheduleLatestBackfill(session);
            if (!session->historyLatestBackfillPending)
                prefetchDaemonHistory(session);
        });
}

} // namespace ketplus
