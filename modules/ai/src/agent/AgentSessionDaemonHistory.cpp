#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/AgentTranscriptPaging.h"
#include "agent/AgentTranscriptWindowBudget.h"
#include "agent/CodexAppServer.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptRowMeasurer.h"
#include "agent/TranscriptTrace.h"

#include <QJsonArray>
#include <QLoggingCategory>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ketplus {
using namespace agent_message;
namespace {

using TurnCursors = QHash<QString, QPair<qint64, qint64>>;

// Runs `work` on the global pool and hands its result to `done` on `context`'s
// thread, unless `context` is gone by then.
template <typename Work, typename Done> void offThread(QObject* context, Work work, Done done) {
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start(
        [guard, work = std::move(work), done = std::move(done)]() mutable {
            auto result = work();
            QMetaObject::invokeMethod(
                guard,
                [guard, done = std::move(done), result = std::move(result)]() mutable {
                    if (guard)
                        done(std::move(result));
                },
                Qt::QueuedConnection);
        });
}

QString nextCursor(const QJsonObject& page, const QString& previous, QString& error) {
    if (!page.value("has_more_before").toBool())
        return {};
    const auto before = page.value("before_cursor").toInteger();
    if (before <= 0 || (!previous.isEmpty() && before >= previous.toLongLong())) {
        error = QStringLiteral("History cursor did not advance. Please retry.");
        return {};
    }
    return QString::number(before);
}

void rememberTurnCursors(TurnCursors& turns, const QJsonObject& page) {
    for (const auto& value : page.value("events").toArray()) {
        const auto event = value.toObject();
        const auto key = daemonTurnId(event.value("turn_sequence").toInteger());
        const auto cursor = event.value("cursor").toInteger();
        auto found = turns.find(key);
        if (found == turns.end())
            turns.insert(key, {cursor, cursor});
        else
            *found = {std::min(found->first, cursor), std::max(found->second, cursor)};
    }
}

// Read-ahead stops once the cached pages hold about this much text.
inline constexpr qsizetype prefetchByteLimit = 48 * 1024 * 1024;

QString pageKey(const bool newer, const QString& cursor, const qint64 snapshotCursor,
                const qint64 projectionRevision) {
    return QStringLiteral("%1:%2:%3:%4")
        .arg(newer ? u'n' : u'o')
        .arg(cursor)
        .arg(snapshotCursor)
        .arg(projectionRevision);
}

// The cursor of the page after `page` in the same direction, or empty at the end.
QString followingCursor(const bool newer, const QJsonObject& page, const QString& cursor) {
    if (!newer) {
        QString error;
        const auto next = nextCursor(page, cursor, error);
        return error.isEmpty() ? next : QString{};
    }
    if (!page.value("has_more_after").toBool())
        return {};
    const auto after = page.value("after_cursor").toInteger();
    return after > cursor.toLongLong() ? QString::number(after) : QString{};
}

// Rough memory held by a projected page: its strings, twice over for the page
// object they came from.
qsizetype messageBytes(const QVariantList& messages) {
    qsizetype bytes = 0;
    for (const auto& value : messages) {
        const auto entry = value.toMap();
        for (auto it = entry.cbegin(); it != entry.cend(); ++it)
            if (it->metaType().id() == QMetaType::QString)
                bytes += it->toString().size() * 2;
    }
    return bytes * 2;
}

struct ProjectedPage final {
    QVariantList projected;
    qsizetype bytes{0};
};

double rowsPerTurn(const QVector<QVariantMap>& messages) {
    QSet<QString> turns;
    for (const auto& entry : messages)
        turns.insert(entry.value("turnId").toString());
    return turns.isEmpty() ? 0.0 : double(messages.size()) / double(turns.size());
}

} // namespace

void AgentSessionManager::loadDaemonHistoryPage(const SessionPtr& session, const bool newer,
                                                const int turnLimitOverride) {
    if (!session || session->threadId.isEmpty() || session->historyLoading)
        return;
    if (session->historyCatchingUp && !newer)
        return;
    if (session->historyDirty && !session->historyCatchingUp) {
        catchUpDaemonHistory(session);
        return;
    }
    if (newer
            ? (!session->historyLoaded || !session->historyHasNewer ||
               session->historyNewerCursor.isEmpty() || !server_->hasDaemonTranscriptAfterCursor())
            : (session->historyLoaded && session->historyCursor.isEmpty())) {
        // There is nothing to page this way, so no read will run and the
        // clearing below is never reached. A notice left standing by a failure
        // in the other direction would then outlive every retry, because the
        // control only ever asks for older rows. Retire it here instead.
        if (!session->historyError.isEmpty()) {
            session->historyError.clear();
            session->historyErrorNewer = false;
            emitHistoryState(session);
        }
        return;
    }
    const bool initial = !newer && !session->historyLoaded;
    if (initial)
        session->daemonLiveRowCursors.clear();
    const auto cursor = initial ? QString{}
                        : newer ? session->historyNewerCursor
                                : session->historyCursor;
    // Publish the newest turn first. The two preceding turns are fetched after
    // that page reaches the model, so a long-running conversation does not make
    // its latest content wait behind older large items. A refresh keeps the
    // previous rows on screen while it reads, so it reads the whole window at
    // once and replaces them in one step instead of two.
    const bool bootstrap =
        initial && transcript_paging::pageCounts().readAhead && !session->historyRefreshingLatest;
    const int turnLimit =
        turnLimitOverride > 0
            ? std::clamp(turnLimitOverride, 1, server_->daemonTranscriptWindowMaxTurns())
        : initial ? (bootstrap ? 1 : transcript_paging::minimumTurns)
                  : transcript_paging::adaptiveTurnLimit(
                        session->historyViewportHeight, session->historyAverageRowHeight,
                        rowsPerTurn(session->messages), server_->daemonTranscriptWindowMaxTurns());
    session->historyLoading = true;
    session->historyLoadingNewer = newer;
    session->historyError.clear();
    session->historyErrorNewer = false;
    session->historyLastUsed = ++historyUseCounter_;
    emitHistoryState(session);
    if (isActiveSession(session) && session->path == activePath_)
        updateRestoreState(session);
    auto batch = std::make_shared<DaemonHistoryBatch>();
    batch->epoch = ++session->historyEpoch;
    batch->initial = initial;
    batch->newer = newer;
    batch->turnLimit = turnLimit;
    const auto pages = transcript_paging::pageCounts();
    batch->pagesWanted = initial ? pages.initial : pages.continuation;
    batch->clock.start();
    const auto epoch = batch->epoch;
    QTimer::singleShot(15000, this, [this, session, epoch, newer] {
        if (session->historyEpoch != epoch || !session->historyLoading)
            return;
        // A stalled catch-up page must release its daemon fence; otherwise
        // historyCatchingUp stays set and every later catch-up is refused.
        cancelDaemonHistoryCatchup(session);
        ++session->historyEpoch;
        session->historyLoading = false;
        session->historyRefreshingLatest = false;
        session->historyError = QStringLiteral("History loading timed out. Please retry.");
        session->historyErrorNewer = newer;
        clearDaemonHistoryCache(*session);
        emitHistoryState(session);
        if (isActiveSession(session) && session->path == activePath_)
            updateRestoreState(session);
    });
    readDaemonHistoryBatchPage(session, batch, cursor, session->historySnapshotCursor,
                               session->historyProjectionRevision);
}

// A batch reads several daemon pages back to back and reaches the model once:
// one insertion, one geometry publication and one reading-position hold per
// batch rather than per page. Each page is still an ordinary fenced page, so a
// failure part way keeps what was already read.
void AgentSessionManager::readDaemonHistoryBatchPage(
    const SessionPtr& session, const std::shared_ptr<DaemonHistoryBatch>& batch,
    const QString& cursor, const qint64 snapshotCursor, const qint64 projectionRevision) {
    batch->fetching = true;
    HistoryPageReceiver received = [this, session, batch, cursor, snapshotCursor,
                                    projectionRevision](const QJsonObject& page,
                                                        const QString& failure, const bool cached,
                                                        const QVariantList& cachedMessages) {
        if (session->historyEpoch != batch->epoch)
            return;
        QString error = failure;
        QString next;
        if (error.isEmpty() && !batch->newer)
            next = nextCursor(page, cursor, error);
        if (error.isEmpty() && batch->newer && page.value("has_more_after").toBool() &&
            page.value("after_cursor").toInteger() <= cursor.toLongLong())
            error = QStringLiteral("History cursor did not advance. Please retry.");
        if (!error.isEmpty()) {
            if (session->historyCatchingUp) {
                failDaemonHistoryCatchup(session, page, error);
                return;
            }
            if (batch->pages > 0) {
                // Keep the pages already read; the next load retries from where
                // they end.
                batch->fetching = false;
                batch->ended = true;
                advanceDaemonHistoryBatch(session, batch);
                return;
            }
            session->historyLoading = false;
            session->historyRefreshingLatest = false;
            const bool snapshotChanged = page.value("error_code") == "snapshot_changed";
            if (snapshotChanged && !batch->initial && session->historyLoaded &&
                !session->messages.isEmpty() && session->historySnapshotRetries < 3) {
                // The projection was rebuilt under a window the reader is still
                // in. Its event cursors stay valid, so paging continues from them
                // against the new snapshot; rows read again merge by message key.
                // Resyncing would replace the window with the newest turns and
                // throw the reader to the end of the conversation.
                ++session->historySnapshotRetries;
                clearDaemonHistoryCache(*session);
                refenceDaemonHistory(session, batch->newer);
                return;
            }
            // The only record of why a window stopped loading: the notice itself
            // names no cause, and the tooltip carrying this string is reachable
            // only while the reader keeps the pointer on the retry control.
            qWarning().noquote() << "KetPlus: transcript history page failed ("
                                 << (batch->newer ? "newer" : "older")
                                 << "), code=" << page.value("error_code").toString("<none>") << ":"
                                 << error;
            session->historyError = error;
            session->historyErrorNewer = batch->newer;
            session->historyResyncRequired = snapshotChanged;
            if (session->historyResyncRequired)
                clearDaemonHistoryCache(*session);
            emitHistoryState(session);
            if (isActiveSession(session) && session->path == activePath_)
                updateRestoreState(session);
            return; // Retain the reading window/anchor on snapshot_changed.
        }
        if (batch->initial && batch->pages == 0)
            session->historyTurnCursors.clear();
        rememberTurnCursors(session->historyTurnCursors, page);
        if (batch->pages == 0) {
            batch->firstPage = page;
            batch->stateSerial = session->stateSerial;
        }
        batch->lastPage = page;
        batch->next = next;
        ++batch->pages;
        if (cached)
            ++batch->cachedPages;
        batch->fetching = false;
        const auto index = batch->readPages.size();
        batch->readPages.append(
            {page, {}, cursor, snapshotCursor, projectionRevision, 0, cached, false});
        if (cached) {
            batch->readPages[index].projected = cachedMessages;
            batch->readPages[index].done = true;
            advanceDaemonHistoryBatch(session, batch);
            return;
        }
        // Building a page's rows costs tens of milliseconds, so it runs on a
        // worker while the page after it is already being read. Pages keep their
        // reading order here, and the batch merges them once they are all built.
        ++batch->projecting;
        offThread(
            this,
            [page] {
                ProjectedPage result;
                result.projected = daemonWindowMessages(page);
                result.bytes = messageBytes(result.projected);
                return result;
            },
            [this, session, batch, index, page, cursor, snapshotCursor,
             projectionRevision](ProjectedPage result) {
                if (session->historyEpoch != batch->epoch)
                    return;
                if (!cursor.isEmpty())
                    cacheDaemonHistoryPage(*session, batch->newer, cursor, snapshotCursor,
                                           projectionRevision, page, result.projected,
                                           result.bytes);
                auto& slot = batch->readPages[index];
                slot.projected = std::move(result.projected);
                slot.bytes = result.bytes;
                slot.done = true;
                --batch->projecting;
                advanceDaemonHistoryBatch(session, batch);
            });
        // The page after this one is read while this one is being built.
        advanceDaemonHistoryBatch(session, batch);
    };
    const auto key = pageKey(batch->newer, cursor, snapshotCursor, projectionRevision);
    if (!cursor.isEmpty()) {
        const auto found = session->historyPageCache.constFind(key);
        if (found != session->historyPageCache.cend()) {
            QTimer::singleShot(0, this, [received, page = found->page, messages = found->messages] {
                received(page, {}, true, messages);
            });
            return;
        }
        if (key ==
            (batch->newer ? session->historyPrefetchKeyNewer : session->historyPrefetchKeyOlder)) {
            // The read-ahead is already fetching this page.
            session->historyPageWaiters[key].append([this, session, batch, cursor, snapshotCursor,
                                                     projectionRevision, received,
                                                     key](const bool cachedNow) {
                if (session->historyEpoch != batch->epoch)
                    return;
                const auto found = session->historyPageCache.constFind(key);
                if (cachedNow && found != session->historyPageCache.cend())
                    received(found->page, {}, true, found->messages);
                else
                    readDaemonHistoryBatchPageUncached(session, batch, cursor, snapshotCursor,
                                                       projectionRevision, received);
            });
            return;
        }
    }
    readDaemonHistoryBatchPageUncached(session, batch, cursor, snapshotCursor, projectionRevision,
                                       received);
}

// A continuation must name the current head and projection revision: the daemon
// rejects a stale revision, and a zero snapshot puts every cursor out of range.
// One newest turn supplies both without touching the reader's window.
void AgentSessionManager::refenceDaemonHistory(const SessionPtr& session, const bool newer) {
    session->historyLoading = true;
    const auto epoch = session->historyEpoch;
    server_->readDaemonTranscriptWindow(
        session->threadId,
        transcript_paging::windowParams(transcript_paging::Direction::Older, {}, 0, 0, 1),
        [this, session, newer, epoch](const QJsonObject& head, const QString& failure) {
            if (session->historyEpoch != epoch)
                return;
            session->historyLoading = false;
            if (!failure.isEmpty()) {
                if (session->historyCatchingUp) {
                    failDaemonHistoryCatchup(session, head, failure);
                    return;
                }
                qWarning().noquote() << "KetPlus: transcript head read failed:" << failure;
                session->historyError = failure;
                session->historyErrorNewer = newer;
                emitHistoryState(session);
                return;
            }
            session->historySnapshotCursor = head.value("snapshot_cursor").toInteger();
            session->historyProjectionRevision = head.value("projection_revision").toInteger();
            loadDaemonHistoryPage(session, newer);
            if (!session->historyLoading)
                emitHistoryState(session);
        },
        session->historyCatchingUp);
}

void AgentSessionManager::readDaemonHistoryBatchPageUncached(
    const SessionPtr& session, const std::shared_ptr<DaemonHistoryBatch>& batch,
    const QString& cursor, const qint64 snapshotCursor, const qint64 projectionRevision,
    const HistoryPageReceiver& received) {
    server_->readDaemonTranscriptWindow(
        session->threadId,
        transcript_paging::windowParams(batch->newer ? transcript_paging::Direction::Newer
                                                     : transcript_paging::Direction::Older,
                                        cursor, snapshotCursor, projectionRevision,
                                        batch->turnLimit),
        [received](const QJsonObject& page, const QString& failure) {
            received(page, failure, false, {});
        });
}

void AgentSessionManager::clearDaemonHistoryCache(Session& session) {
    // Lazy loads waiting on a read-ahead page read it themselves.
    const auto waiters = std::exchange(session.historyPageWaiters, {});
    session.historyPrefetchKeyOlder.clear();
    session.historyPrefetchKeyNewer.clear();
    for (const auto& list : waiters)
        for (const auto& waiter : list)
            QTimer::singleShot(0, [waiter] { waiter(false); });
    session.historyPageCache.clear();
    session.historyPageCacheBytes = 0;
    session.historyPageCacheSnapshot = 0;
    session.historyPageCacheRevision = 0;
    ++session.historyPrefetchGeneration;
    session.historyPrefetchingOlder = false;
    session.historyPrefetchingNewer = false;
}

void AgentSessionManager::cacheDaemonHistoryPage(Session& session, const bool newer,
                                                 const QString& cursor, const qint64 snapshotCursor,
                                                 const qint64 projectionRevision,
                                                 const QJsonObject& page,
                                                 const QVariantList& messages,
                                                 const qsizetype bytes) {
    if (snapshotCursor <= 0)
        return;
    // Pages are only valid within the snapshot fence they were read in.
    if (session.historyPageCacheSnapshot != snapshotCursor ||
        session.historyPageCacheRevision != projectionRevision) {
        clearDaemonHistoryCache(session);
        session.historyPageCacheSnapshot = snapshotCursor;
        session.historyPageCacheRevision = projectionRevision;
    }
    const auto key = pageKey(newer, cursor, snapshotCursor, projectionRevision);
    if (session.historyPageCache.contains(key))
        return;
    session.historyPageCache.insert(key, {page, messages, bytes});
    session.historyPageCacheBytes += bytes;
}

// Reads only the adjacent page in each direction. Another window publication
// advances these edges; opening a conversation must not drain its entire history.
// Each page is projected and measured on worker threads, so a later lazy load
// only has to insert rows.
void AgentSessionManager::prefetchDaemonHistory(const SessionPtr& session) {
    if (!session || session->threadId.isEmpty() || !session->historyLoaded ||
        session->historyDirty || !server_->hasDaemonTranscriptWindow() ||
        !isActiveSession(session) || !transcript_paging::pageCounts().readAhead)
        return;
    prefetchDaemonHistoryPage(session, false);
    if (server_->hasDaemonTranscriptAfterCursor())
        prefetchDaemonHistoryPage(session, true);
}

void AgentSessionManager::releasePageWaiters(Session& session, const bool newer,
                                             const bool cached) {
    auto& key = newer ? session.historyPrefetchKeyNewer : session.historyPrefetchKeyOlder;
    const auto waiters = session.historyPageWaiters.take(key);
    key.clear();
    for (const auto& waiter : waiters)
        waiter(cached);
}

void AgentSessionManager::prefetchDaemonHistoryPage(const SessionPtr& session, const bool newer) {
    auto& inFlight = newer ? session->historyPrefetchingNewer : session->historyPrefetchingOlder;
    const auto snapshot = session->historySnapshotCursor;
    const auto revision = session->historyProjectionRevision;
    if (inFlight || snapshot <= 0 || !isActiveSession(session) || session->path != activePath_)
        return;
    if (session->historyPageCacheSnapshot != snapshot ||
        session->historyPageCacheRevision != revision) {
        clearDaemonHistoryCache(*session);
        session->historyPageCacheSnapshot = snapshot;
        session->historyPageCacheRevision = revision;
    }
    QString cursor = newer ? (session->historyHasNewer ? session->historyNewerCursor : QString{})
                           : session->historyCursor;
    if (cursor.isEmpty() || session->historyPageCacheBytes >= prefetchByteLimit ||
        session->historyPageCache.contains(pageKey(newer, cursor, snapshot, revision)))
        return;
    inFlight = true;
    const auto key = pageKey(newer, cursor, snapshot, revision);
    (newer ? session->historyPrefetchKeyNewer : session->historyPrefetchKeyOlder) = key;
    const auto generation = session->historyPrefetchGeneration;
    const int turnLimit = transcript_paging::adaptiveTurnLimit(
        session->historyViewportHeight, session->historyAverageRowHeight,
        rowsPerTurn(session->messages), server_->daemonTranscriptWindowMaxTurns());
    server_->readDaemonTranscriptWindow(
        session->threadId,
        transcript_paging::windowParams(newer ? transcript_paging::Direction::Newer
                                              : transcript_paging::Direction::Older,
                                        cursor, snapshot, revision, turnLimit),
        [this, session, newer, cursor, snapshot, revision, generation](const QJsonObject& page,
                                                                       const QString& failure) {
            if (session->historyPrefetchGeneration != generation)
                return;
            if (!failure.isEmpty()) {
                if (page.value("error_code") == "snapshot_changed")
                    clearDaemonHistoryCache(*session);
                else {
                    (newer ? session->historyPrefetchingNewer : session->historyPrefetchingOlder) =
                        false;
                    releasePageWaiters(*session, newer, false);
                }
                return; // The next window change retries.
            }
            offThread(
                this,
                [page] {
                    ProjectedPage result;
                    result.projected = daemonWindowMessages(page);
                    result.bytes = messageBytes(result.projected);
                    return result;
                },
                [this, session, newer, cursor, snapshot, revision, generation,
                 page](ProjectedPage result) {
                    if (session->historyPrefetchGeneration != generation)
                        return;
                    cacheDaemonHistoryPage(*session, newer, cursor, snapshot, revision, page,
                                           result.projected, result.bytes);
                    releasePageWaiters(*session, newer, true);
                    TranscriptRowMeasurer::premeasure(
                        result.projected, this, [session, newer, generation] {
                            if (session->historyPrefetchGeneration != generation)
                                return;
                            (newer ? session->historyPrefetchingNewer
                                   : session->historyPrefetchingOlder) = false;
                        });
                });
        });
}

// Drives a batch: reads the next page as soon as the one before it has arrived,
// and merges the pages once they are all read and built.
void AgentSessionManager::advanceDaemonHistoryBatch(
    const SessionPtr& session, const std::shared_ptr<DaemonHistoryBatch>& batch) {
    if (session->historyEpoch != batch->epoch || batch->fetching || batch->readPages.isEmpty())
        return;
    if (!batch->ended) {
        const auto& last = batch->readPages.constLast();
        const QString following =
            batch->newer ? followingCursor(true, last.page, last.cursor) : batch->next;
        // The first `pagesWanted` pages are read whatever they hold, so the page
        // after one of them is asked for before it has been built. Reading past
        // them weighs the rows read so far, which every page must be built for.
        const bool fills = !batch->initial && transcript_paging::pageCounts().fillBuffer;
        if (fills && batch->pages >= batch->pagesWanted && batch->projecting > 0)
            return; // The row count is not known yet; a projection will call back.
        qsizetype rows = 0;
        qsizetype estimated = 0;
        for (const auto& read : batch->readPages) {
            rows += read.projected.size();
            // A page still being built holds at most one row per event, so the
            // window limit is never overrun while waiting for the real count.
            estimated +=
                read.done ? read.projected.size() : read.page.value("events").toArray().size();
        }
        const bool wantsMore =
            fills ? transcript_paging::batchWantsMore(batch->pages, batch->pagesWanted, rows,
                                                      session->historyViewportHeight,
                                                      session->historyHeightPerMessage)
                  : batch->pages < batch->pagesWanted;
        const bool more = wantsMore && !following.isEmpty() &&
                          estimated + session->messages.size() < historyWindowLimit(*session);
        if (more) {
            // Every later page is fenced to the snapshot the first one read.
            readDaemonHistoryBatchPage(session, batch, following,
                                       last.page.value("snapshot_cursor").toInteger(),
                                       last.page.value("projection_revision").toInteger());
            return;
        }
        batch->ended = true;
    }
    if (batch->projecting > 0)
        return;
    // Merging is cheap next to building; the pages fold together in reading
    // order, an older page ahead of what is held and a newer page behind it.
    QVariantList messages;
    for (qsizetype index = 0; index < batch->readPages.size(); ++index) {
        auto& read = batch->readPages[index];
        messages = index == 0     ? std::move(read.projected)
                   : batch->newer ? mergeDaemonHistory(messages, read.projected)
                                  : mergeDaemonHistory(read.projected, messages);
    }
    batch->messages = std::move(messages);
    finishDaemonHistoryBatch(session, batch);
}

void AgentSessionManager::finishDaemonHistoryBatch(
    const SessionPtr& session, const std::shared_ptr<DaemonHistoryBatch>& batch) {
    // Measure the new rows on worker threads first, so they reach the model with
    // their heights, then wait for the reader to stop scrolling: inserting rows
    // costs the GUI thread a frame or more, and it must not land mid-gesture.
    batch->readMs = batch->clock.elapsed();
    TranscriptRowMeasurer::premeasure(batch->messages, this, [this, session, batch] {
        if (session->historyEpoch != batch->epoch)
            return;
        batch->measuredMs = batch->clock.elapsed();
        whenTranscriptIdle([this, session, batch] {
            if (session->historyEpoch != batch->epoch)
                return;
            prepareDaemonHistoryBatch(session, batch);
        });
    });
}

void AgentSessionManager::setTranscriptExtent(const double renderedHeight,
                                              const double viewportHeight) {
    const auto session = sessions_.value(normalizedPath(activePath_));
    if (!session || session->messages.isEmpty())
        return;
    if (renderedHeight > 0)
        session->historyHeightPerMessage = renderedHeight / double(session->messages.size());
    // Only ever grows while the window lasts: a budget recomputed from the rows
    // it just trimmed shrinks the window, which changes the next estimate, and
    // the window swings between sizes on every page.
    session->historyMessageBudget =
        std::max(session->historyMessageBudget,
                 transcript_window::messageBudget(renderedHeight, int(session->messages.size()),
                                                  viewportHeight));
}

void AgentSessionManager::setTranscriptInputActive(const bool active) {
    transcriptInputActive_ = active;
    if (active) {
        idleSettle_.stop();
        // The deadline counts from the last sign of scrolling, so a long run of
        // gestures is never interrupted by it.
        if (!idleWaiters_.isEmpty())
            idleDeadline_.start();
        return;
    }
    // Scrolling reads as stopped for an instant between a finger lifting and
    // inertia starting, and whenever a view restarts its own idle timer. Only a
    // pause that lasts is a gap history may be inserted in.
    if (!idleWaiters_.isEmpty())
        idleSettle_.start();
}

void AgentSessionManager::runIdleWaiters() {
    idleSettle_.stop();
    idleDeadline_.stop();
    const auto waiting = std::exchange(idleWaiters_, {});
    for (const auto& waiter : waiting)
        waiter();
}

void AgentSessionManager::whenTranscriptIdle(std::function<void()> run) {
    if (!transcriptInputActive_ && !idleSettle_.isActive() && idleWaiters_.isEmpty()) {
        run();
        return;
    }
    idleWaiters_.append(std::move(run));
    if (!transcriptInputActive_ && !idleSettle_.isActive())
        idleSettle_.start();
    // A view torn down mid-gesture never reports the end of it, and nothing
    // reports scrolling after that; history is not held back forever on its
    // account.
    if (!idleDeadline_.isActive())
        idleDeadline_.start();
}

AgentSessionManager::PreparedDaemonWindow AgentSessionManager::prepareDaemonWindow(
    const QVariantList& batchMessages, const QVector<QVariantMap>& basis,
    const QSet<QString>& pendingQuestionKeys, const bool initial, const bool newer,
    const QHash<QString, qint64>& liveRowCursors, const qint64 snapshotCursor,
    const int messageLimit, const qsizetype byteLimit, const QString& readingKey) {
    PreparedDaemonWindow prepared;
    prepared.basis = basis;
    prepared.readingKey = readingKey;
    QVariantList projected = batchMessages;
    QVariantList current;
    current.reserve(basis.size());
    for (const auto& message : basis)
        current.append(message);
    if (initial) {
        // Keep actual outstanding requests. The projection holds a settled approval's note,
        // and a hidden half of one still waiting; the card joins it, and a receipt the
        // journal holds settles it instead.
        QHash<QString, int> projectedKeys;
        for (int index = 0; index < projected.size(); ++index)
            projectedKeys.insert(projected.at(index).toMap().value("messageKey").toString(), index);
        for (const auto& message : basis) {
            const auto key = message.value("messageKey").toString();
            if (!pendingQuestionKeys.contains(key))
                continue;
            const int index = projectedKeys.value(key, -1);
            if (index < 0 || message.value("subtype") != "approval") {
                projected.append(message);
                continue;
            }
            const auto joined =
                agent_message::joinedApprovalRows(projected.at(index).toMap(), message);
            if (!joined.value("running").toBool())
                prepared.settledPending.insert(key);
            projected[index] = joined;
        }
        // The daemon starts dispatching buffered live events after returning
        // the page, while this projection still runs on a worker. Only overlay
        // rows whose event cursor is newer than the page's snapshot; an older
        // cached row must never replace the freshly read version.
        QVariantList live;
        for (const auto& message : basis)
            if (liveRowCursors.value(message.value("messageKey").toString()) > snapshotCursor)
                live.append(message);
        if (!live.isEmpty())
            projected = mergeDaemonHistory(projected, live);
    } else if (newer)
        projected = mergeDaemonHistory(current, projected);
    else
        projected = mergeDaemonHistory(projected, current);
    prepared.bounded.reserve(projected.size());
    for (const auto& value : projected)
        prepared.bounded.append(value.toMap());
    prepared.untrimmed = prepared.bounded;
    prepared.trim =
        transcript_window::trim(prepared.bounded,
                                initial || newer ? transcript_window::RetainedEdge::Newest
                                                 : transcript_window::RetainedEdge::Oldest,
                                messageLimit, byteLimit, initial ? QString{} : readingKey);
    QHash<QString, int> oldIndexes;
    for (int index = 0; index < basis.size(); ++index)
        oldIndexes.insert(basis.at(index).value("messageKey").toString(), index);
    for (const auto& entry : std::as_const(prepared.bounded)) {
        prepared.projected.append(entry);
        prepared.retainedTurns.insert(entry.value("turnId").toString());
        if (initial)
            continue;
        const auto oldIndex = oldIndexes.value(entry.value("messageKey").toString(), -1);
        if (oldIndex < 0 && newer)
            prepared.appended = true;
        else if (oldIndex < 0)
            prepared.prefix.append(entry);
        else if (basis.at(oldIndex) != entry)
            prepared.updates.append({oldIndex, entry});
    }
    return prepared;
}

void AgentSessionManager::prepareDaemonHistoryBatch(
    const SessionPtr& session, const std::shared_ptr<DaemonHistoryBatch>& batch) {
    QSet<QString> pendingKeys;
    for (auto it = session->pendingQuestionRequests.cbegin();
         it != session->pendingQuestionRequests.cend(); ++it)
        pendingKeys.insert(it.key());
    // Merging, trimming and comparing a window of several hundred rows is the
    // other half of a page's GUI-thread cost; it runs against a snapshot of the
    // session's messages, and the snapshot is checked before it is applied.
    offThread(
        this,
        [messages = batch->messages, basis = session->messages, pendingKeys,
         liveRowCursors = session->daemonLiveRowCursors,
         snapshotCursor = batch->firstPage.value("snapshot_cursor").toInteger(),
         initial = batch->initial, newer = batch->newer,
         limit = historyBatchLimit(*session, *batch),
         bytes = historyBatchByteLimit(*session, *batch), readingKey = session->historyReadingKey] {
            return prepareDaemonWindow(messages, basis, pendingKeys, initial, newer, liveRowCursors,
                                       snapshotCursor, limit, bytes, readingKey);
        },
        [this, session, batch](PreparedDaemonWindow prepared) {
            if (session->historyEpoch != batch->epoch)
                return;
            batch->preparedMs = batch->clock.elapsed();
            whenTranscriptIdle([this, session, batch, prepared = std::move(prepared)]() mutable {
                if (session->historyEpoch != batch->epoch)
                    return;
                batch->idleMs = batch->clock.elapsed();
                // A shared copy detaches on any write, so an unchanged buffer
                // means nothing streamed into the session since the snapshot.
                if (prepared.basis.constData() != session->messages.constData() ||
                    prepared.readingKey != session->historyReadingKey) {
                    QSet<QString> pendingKeys;
                    for (auto it = session->pendingQuestionRequests.cbegin();
                         it != session->pendingQuestionRequests.cend(); ++it)
                        pendingKeys.insert(it.key());
                    prepared = prepareDaemonWindow(
                        batch->messages, session->messages, pendingKeys, batch->initial,
                        batch->newer, session->daemonLiveRowCursors,
                        batch->firstPage.value("snapshot_cursor").toInteger(),
                        historyBatchLimit(*session, *batch),
                        historyBatchByteLimit(*session, *batch), session->historyReadingKey);
                }
                applyDaemonHistoryBatch(session, *batch, prepared);
            });
        });
}

void AgentSessionManager::applyDaemonHistoryBatch(const SessionPtr& session,
                                                  const DaemonHistoryBatch& batch,
                                                  const PreparedDaemonWindow& prepared) {
    session->historyLoading = false;
    session->historyRefreshingLatest = false;
    // The journal decided these while the cards waited; they take no answer now.
    bool settledCards = false;
    for (const auto& key : prepared.settledPending)
        settledCards = session->pendingQuestionRequests.remove(key) > 0 || settledCards;
    if (settledCards && session->pendingQuestionRequests.isEmpty())
        updateState(session,
                    session->turnId.isEmpty() ? QStringLiteral("Ready")
                                              : QStringLiteral("Working…"),
                    !session->turnId.isEmpty());
    const bool initial = batch.initial;
    const bool newer = batch.newer;
    const QString& next = batch.next;
    const QJsonObject& firstPage = batch.firstPage;
    const QJsonObject& lastPage = batch.lastPage;
    const auto& bounded = prepared.bounded;
    const auto& untrimmed = prepared.untrimmed;
    const auto& trim = prepared.trim;
    if (session->historyCatchingUp && trim.changed) {
        // Catch-up must never evict the retained reader to satisfy a page budget.
        // Beyond the temporary source bound, offer an explicit reload instead.
        failDaemonHistoryCatchup(
            session, {}, QStringLiteral("Too much new history. Reload latest to continue."));
        return;
    }
    const auto& projected = prepared.projected;
    const auto& retainedTurns = prepared.retainedTurns;
    const auto& prefix = prepared.prefix;
    const bool appended = prepared.appended;
    if (!newer) {
        session->historyCursor = next;
        if (initial) {
            session->historyHasNewer = false;
            session->historyNewerCursor.clear();
        }
    } else {
        session->historyHasNewer = lastPage.value("has_more_after").toBool();
        session->historyNewerCursor =
            session->historyHasNewer ? QString::number(lastPage.value("after_cursor").toInteger())
                                     : QString{};
    }
    if (!bounded.isEmpty()) {
        if (trim.removedNewer) {
            const QString lastKey = bounded.constLast().value("messageKey").toString();
            qsizetype end = 0;
            while (end < untrimmed.size() &&
                   untrimmed.at(end).value("messageKey").toString() != lastKey)
                ++end;
            resumeNewerAfterTrim(session, untrimmed, end + 1);
        }
        if (trim.removedOlder)
            resumeOlderAfterTrim(session, untrimmed, bounded);
    }
    if (initial) {
        session->daemonTurnSequence = firstPage.value("current_turn_sequence").toInteger();
        session->historyMessageBudget = transcript_window::maximumMessages;
        session->historyLatestBackfillPending = transcript_paging::pageCounts().readAhead &&
                                                !next.isEmpty() &&
                                                batch.turnLimit < transcript_paging::minimumTurns;
    }
    installDaemonWindow(session, bounded);
    if (initial)
        session->daemonLiveRowCursors.clear();
    if (initial) {
        // An assistant delta may arrive after the page snapshot but before its
        // row has been installed. The live handler holds that text until a
        // public item is known; the fresh page supplies the missing row.
        const auto deferredItems = session->deferredAgentOrder;
        for (const auto& itemId : deferredItems) {
            const int index = messageIndexForItem(session, itemId);
            if (index < 0 || index >= session->messages.size() ||
                session->messages.at(index).value("kind") != QStringLiteral("assistant"))
                continue;
            QString text;
            for (const auto& delta : session->daemonDeferredDeltasByItem.take(itemId))
                if (delta.first > firstPage.value("snapshot_cursor").toInteger())
                    text += delta.second;
            session->deferredAgentTextByItem.remove(itemId);
            session->deferredAgentOrder.removeAll(itemId);
            appendAgentDelta(session, itemId, text);
        }
    }
    // An older page grown past the budget during a running turn is bounded
    // once the reader is idle; scrolling stopping starts the same trim.
    if (!initial && !newer && session->running && !transcriptScrolling_ &&
        isActiveSession(session) && bounded.size() > session->historyMessageBudget)
        historyTrimSettle_.start();
    if (!initial && isActiveSession(session))
        for (const auto& update : prepared.updates)
            emit messageUpdated(session->path, update.first, update.second);
    if (!initial) {
        // Bound the cursor index to the loaded window.
        for (auto it = session->historyTurnCursors.begin();
             it != session->historyTurnCursors.end();) {
            if (retainedTurns.contains(it.key()))
                ++it;
            else
                it = session->historyTurnCursors.erase(it);
        }
    }
    session->historySnapshotRetries = 0;
    session->historySnapshotCursor = lastPage.value("snapshot_cursor").toInteger();
    session->historyProjectionRevision = lastPage.value("projection_revision").toInteger();
    session->historyLoaded = true;
    if (initial || session->historyCatchingUp) {
        // A fresh newest window closes any gap left by an expired replay cursor.
        session->historyResyncRequired = false;
        session->historyError.clear();
        session->turnId = firstPage.value("active_turn_id").toString();
        // Later pages and their measurement take time; a live update that
        // arrived meanwhile is newer than the state this window was read with.
        if (session->stateSerial == batch.stateSerial) {
            const auto display = daemonDisplayState(firstPage.value("state").toString());
            session->running = display.running;
            session->status = display.status;
        }
        // Keep newer live state, but derive its compaction overlay from the reconciled rows.
        if (session->running && !session->cancelRequested) {
            if (!session->activeCompactions.isEmpty())
                session->status = QStringLiteral("Compacting context…");
            else if (session->status == QStringLiteral("Compacting context…"))
                session->status = QStringLiteral("Working…");
        }
        if (isActiveSession(session)) {
            if (initial)
                emit messagesReset(session->path, projected);
            emit sessionStateChanged(session->path, session->title, session->status,
                                     session->running);
        }
    }
    if (!initial && isActiveSession(session)) {
        if (trim.changed || appended)
            emit messagesWindowReset(session->path, projected);
        else if (!prefix.isEmpty())
            emit messagesPrepended(session->path, prefix);
    }
    session->historyLastBatch = {{"newer", newer},
                                 {"pages", batch.pages},
                                 {"cachedPages", batch.cachedPages},
                                 {"messages", batch.messages.size()},
                                 {"readMs", batch.readMs},
                                 {"measuredMs", batch.measuredMs},
                                 {"preparedMs", batch.preparedMs},
                                 {"idleMs", batch.idleMs},
                                 {"appliedMs", batch.clock.elapsed()},
                                 {"cacheBytes", session->historyPageCacheBytes}};
    emitHistoryState(session);
    if (isActiveSession(session) && session->path == activePath_)
        updateRestoreState(session);
    pruneDaemonHistory();
    if (session->historyCatchingUp)
        continueDaemonHistoryCatchup(session, lastPage);
    else if (session->historyDirty)
        catchUpDaemonHistory(session);
    else
        scheduleLatestBackfill(session);
    if (!session->historyLatestBackfillPending)
        prefetchDaemonHistory(session);
}

void AgentSessionManager::scheduleLatestBackfill(const SessionPtr& session) {
    if (!session || !session->historyLatestBackfillPending || !session->historyLoaded ||
        session->historyLoading || session->historyCatchingUp || session->historyDirty ||
        session->historyCursor.isEmpty() || !isActiveSession(session) ||
        session->path != activePath_)
        return;
    const auto epoch = session->historyEpoch;
    QTimer::singleShot(0, this, [this, session, epoch] {
        if (session->historyEpoch != epoch || !session->historyLatestBackfillPending ||
            !isActiveSession(session) || session->path != activePath_ || session->historyLoading ||
            session->historyCatchingUp || session->historyDirty)
            return;
        session->historyLatestBackfillPending = false;
        loadDaemonHistoryPage(session, false, 2);
    });
}

int AgentSessionManager::historyWindowLimit(const Session& session) const {
    // Scrolling reads as stopped for a moment between wheel bursts and while a
    // glide is held at an edge; the window only counts as idle once the trim
    // settle delay has passed without scrolling resuming.
    return transcriptScrolling_ || historyTrimSettle_.isActive()
               ? std::max(session.historyMessageBudget, transcript_window::scrollingMessageLimit)
               : session.historyMessageBudget;
}

qsizetype AgentSessionManager::historyWindowByteLimit() const {
    return transcriptScrolling_ || historyTrimSettle_.isActive()
               ? transcript_window::scrollingUtf8Bytes
               : transcript_window::maximumUtf8Bytes;
}

// While a turn runs, an older page must not evict the newer rows: the live
// turn is appended after them. The page grows the window instead, and the idle
// trim then bounds it around the reader, keeping the live tail.
int AgentSessionManager::historyBatchLimit(const Session& session,
                                           const DaemonHistoryBatch& batch) const {
    return session.historyCatchingUp || (session.running && !batch.initial && !batch.newer)
               ? std::max(historyWindowLimit(session), transcript_window::scrollingMessageLimit)
               : historyWindowLimit(session);
}

qsizetype AgentSessionManager::historyBatchByteLimit(const Session& session,
                                                     const DaemonHistoryBatch& batch) const {
    return session.historyCatchingUp || (session.running && !batch.initial && !batch.newer)
               ? transcript_window::scrollingUtf8Bytes
               : historyWindowByteLimit();
}

namespace {
// The view reads rendered rows; a long answer renders as blocks keyed
// "<messageKey>:block:<offset>". The session keeps source rows only.
QString sourceReadingKey(const QString& readingKey) {
    const qsizetype block = readingKey.indexOf(QStringLiteral(":block:"));
    return block > 0 ? readingKey.left(block) : readingKey;
}
} // namespace

void AgentSessionManager::setTranscriptScrolling(const bool scrolling, const QString& readingKey) {
    transcriptScrolling_ = scrolling;
    if (scrolling) {
        historyTrimSettle_.stop();
        return;
    }
    if (const auto session = sessions_.value(normalizedPath(activePath_));
        session && !readingKey.isEmpty())
        session->historyReadingKey = sourceReadingKey(readingKey);
    historyTrimSettle_.start();
}

void AgentSessionManager::setTranscriptReadingKey(const QString& readingKey) {
    if (const auto session = sessions_.value(normalizedPath(activePath_)))
        session->historyReadingKey = sourceReadingKey(readingKey);
}

void AgentSessionManager::installDaemonWindow(const SessionPtr& session,
                                              const QVector<QVariantMap>& bounded) {
    session->messages.clear();
    session->messageIndexByItem.clear();
    session->loadedIndexByItem.clear();
    session->messages.reserve(bounded.size());
    for (const auto& entry : bounded) {
        const auto index = session->messages.size();
        session->messages.append(entry);
        const auto itemId = entry.value("itemId").toString();
        // Rows from earlier turns stay reachable through loadedIndexByItem (consulted
        // by messageIndexForItem), so late updates edit them in place.
        if (!itemId.isEmpty())
            session->loadedIndexByItem.insert(itemId, index);
        if (!itemId.isEmpty() &&
            entry.value("turnId").toString() == daemonTurnId(session->daemonTurnSequence)) {
            session->messageIndexByItem.insert(itemId, index);
            // Inactive content events are caught up as a projected window, not replayed
            // through handleItem. Reconcile each retained compaction at that same boundary.
            if (entry.value("subtype") == QStringLiteral("compaction")) {
                if (entry.value("running").toBool())
                    session->activeCompactions.insert(itemId);
                else
                    session->activeCompactions.remove(itemId);
            }
        }
    }
}

void AgentSessionManager::resumeNewerAfterTrim(const SessionPtr& session,
                                               const QVector<QVariantMap>& untrimmed,
                                               const qsizetype retained) {
    const auto turnCursor = [&](const QVariantMap& entry) {
        return session->historyTurnCursors.value(entry.value("turnId").toString(), {-1, -1});
    };
    // Resume newer paging at the start of the first evicted turn; a partially
    // retained turn is re-read whole and merged by message key.
    const auto first = turnCursor(untrimmed.at(retained));
    qint64 after = first.first > 1 ? first.first - 1 : 0;
    if (after <= 0) {
        // Unknown evicted turn: continue after the newest known retained event;
        // re-read rows merge by message key.
        for (qsizetype index = 0; index < retained; ++index)
            after = std::max(after, turnCursor(untrimmed.at(index)).second);
    }
    session->historyHasNewer = true;
    session->historyNewerCursor = after > 0 ? QString::number(after) : QString{};
}

void AgentSessionManager::resumeOlderAfterTrim(const SessionPtr& session,
                                               const QVector<QVariantMap>& untrimmed,
                                               const QVector<QVariantMap>& bounded) {
    const auto turnCursor = [&](const QVariantMap& entry) {
        return session->historyTurnCursors.value(entry.value("turnId").toString(), {-1, -1});
    };
    const auto& firstRetained = bounded.constFirst();
    qsizetype removed = 0;
    while (removed < untrimmed.size() &&
           untrimmed.at(removed).value("messageKey") != firstRetained.value("messageKey"))
        ++removed;
    Q_ASSERT(removed > 0 && removed < untrimmed.size());
    const auto retainedTurn = turnCursor(firstRetained);
    const bool splitTurn =
        untrimmed.at(removed - 1).value("turnId") == firstRetained.value("turnId");
    qint64 before = splitTurn ? retainedTurn.second + 1 : retainedTurn.first;
    if (before <= 0) {
        before = std::numeric_limits<qint64>::max();
        for (const auto& entry : bounded) {
            const auto known = turnCursor(entry);
            if (known.first > 0)
                before = std::min(before, known.first);
        }
    }
    if (before > 0 && before != std::numeric_limits<qint64>::max())
        session->historyCursor = QString::number(before);
}

void AgentSessionManager::trimDaemonHistoryWindow(const SessionPtr& session,
                                                  const bool emitWindow) {
    if (!session || !server_->hasDaemonTranscriptWindow() || !session->historyLoaded ||
        session->historyLoading || session->historyCatchingUp || transcriptScrolling_ ||
        session->messages.size() <= session->historyMessageBudget)
        return;
    QElapsedTimer clock;
    clock.start();
    const int limit = session->historyMessageBudget;
    const int count = int(session->messages.size());
    // No key: the reader follows the tail.
    int reading = session->historyReadingKey.isEmpty() ? count - 1 : -1;
    for (int index = 0; reading < 0 && index < count; ++index)
        if (session->messages.at(index).value("messageKey").toString() ==
            session->historyReadingKey)
            reading = index;
    if (reading < 0) {
        // Centring on the newest rows instead would evict the ones on screen.
        // Keep the window as it is until the reader's row is known again.
        transcript_trace::write(QStringLiteral("history-trim-skipped"),
                                {{QStringLiteral("readingKey"), session->historyReadingKey},
                                 {QStringLiteral("count"), count}});
        return;
    }
    // Newest rows past the reader go first, then the oldest, leaving the budget
    // centred on the reader. Each side keeps at least trimMarginViewports of
    // rendered height: most messages can fold into activity groups, and half a
    // count budget of them may be only a screen or two, which the reader then
    // has to wait for page by page.
    int half = limit / 2;
    if (session->historyViewportHeight > 0 && session->historyHeightPerMessage > 0)
        half = std::max(half, int(std::ceil(transcript_window::trimMarginViewports *
                                            session->historyViewportHeight /
                                            session->historyHeightPerMessage)));
    // While a turn runs, rows past a reader who scrolled up are where the live
    // turn is being appended. Evicting them would leave a gap between the
    // retained window and every event still to come.
    const int keepThrough =
        session->running ? count : std::clamp(reading + half, std::min(limit, count), count);
    const int keepFrom = std::clamp(reading - half, 0, keepThrough);
    constexpr auto countOnly = transcript_window::unboundedUtf8Bytes;
    const QVector<QVariantMap> untrimmed = session->messages;
    QVector<QVariantMap> bounded = untrimmed;
    auto newerTrim = transcript_window::trim(bounded, transcript_window::RetainedEdge::Oldest,
                                             keepThrough, countOnly);
    if (newerTrim.removedNewer)
        resumeNewerAfterTrim(session, untrimmed, bounded.size());
    const QVector<QVariantMap> afterNewer = bounded;
    auto olderTrim =
        transcript_window::trim(bounded, transcript_window::RetainedEdge::Newest,
                                std::max(limit, int(bounded.size()) - keepFrom), countOnly);
    // The byte bound then evicts around the reader's turn, never into it, and
    // never the newer rows while a turn runs. Applied as a plain edge trim it
    // once evicted the reader's rows and the live tail.
    const QString readingTurn = untrimmed.at(reading).value("turnId").toString();
    const QString readingKey = untrimmed.at(reading).value("messageKey").toString();
    int turnFrom = -1;
    int turnTo = -1;
    for (int index = 0; index < bounded.size(); ++index) {
        const auto& entry = bounded.at(index);
        const bool inTurn = readingTurn.isEmpty()
                                ? entry.value("messageKey").toString() == readingKey
                                : entry.value("turnId").toString() == readingTurn;
        if (!inTurn)
            continue;
        if (turnFrom < 0)
            turnFrom = index;
        turnTo = index + 1;
    }
    if (turnFrom >= 0) {
        const auto cut = transcript_window::byteCut(bounded, turnFrom, turnTo, !session->running);
        if (cut.fromOldest > 0) {
            const auto bytesOlder =
                transcript_window::trim(bounded, transcript_window::RetainedEdge::Newest,
                                        int(bounded.size()) - cut.fromOldest, countOnly);
            olderTrim.changed = olderTrim.changed || bytesOlder.changed;
            olderTrim.removedOlder = olderTrim.removedOlder || bytesOlder.removedOlder;
        }
        if (olderTrim.removedOlder && !bounded.isEmpty())
            resumeOlderAfterTrim(session, afterNewer, bounded);
        if (cut.fromNewest > 0) {
            const QVector<QVariantMap> beforeNewerBytes = bounded;
            const auto bytesNewer =
                transcript_window::trim(bounded, transcript_window::RetainedEdge::Oldest,
                                        int(bounded.size()) - cut.fromNewest, countOnly);
            if (bytesNewer.removedNewer)
                resumeNewerAfterTrim(session, beforeNewerBytes, bounded.size());
            newerTrim.changed = newerTrim.changed || bytesNewer.changed;
            newerTrim.removedNewer = newerTrim.removedNewer || bytesNewer.removedNewer;
        }
    } else if (olderTrim.removedOlder && !bounded.isEmpty()) {
        resumeOlderAfterTrim(session, afterNewer, bounded);
    }
    if (!newerTrim.changed && !olderTrim.changed)
        return;
    transcript_trace::write(QStringLiteral("history-trim"),
                            {{QStringLiteral("readingKey"), session->historyReadingKey},
                             {QStringLiteral("reading"), reading},
                             {QStringLiteral("count"), count},
                             {QStringLiteral("kept"), int(bounded.size())},
                             {QStringLiteral("removedNewer"), newerTrim.removedNewer},
                             {QStringLiteral("removedOlder"), olderTrim.removedOlder},
                             {QStringLiteral("running"), session->running},
                             {QStringLiteral("trimMs"), clock.elapsed()}});
    QSet<QString> retainedTurns;
    QVariantList projected;
    projected.reserve(bounded.size());
    for (const auto& entry : std::as_const(bounded)) {
        retainedTurns.insert(entry.value("turnId").toString());
        projected.append(entry);
    }
    installDaemonWindow(session, bounded);
    for (auto it = session->historyTurnCursors.begin(); it != session->historyTurnCursors.end();) {
        if (retainedTurns.contains(it.key()))
            ++it;
        else
            it = session->historyTurnCursors.erase(it);
    }
    if (emitWindow && isActiveSession(session)) {
        emit messagesWindowReset(session->path, projected);
        emitHistoryState(session);
    }
}

void AgentSessionManager::pruneDaemonHistory() {
    if (!server_->hasDaemonTranscriptWindow())
        return;
    QVector<SessionPtr> cached;
    qsizetype bytes = 0;
    for (const auto& session : sessionsByThread_) {
        if (session->messages.isEmpty())
            continue;
        cached.append(session);
        for (const auto& entry : session->messages)
            for (auto it = entry.cbegin(); it != entry.cend(); ++it)
                if (it->metaType().id() == QMetaType::QString)
                    bytes += it->toString().size() * 2;
    }
    // Read-ahead pages only serve the conversation on screen.
    for (const auto& session : sessionsByThread_)
        if (!(isActiveSession(session) && session->path == activePath_) &&
            !session->historyPageCache.isEmpty())
            clearDaemonHistoryCache(*session);
    std::sort(cached.begin(), cached.end(), [](const SessionPtr& a, const SessionPtr& b) {
        return a->historyLastUsed < b->historyLastUsed;
    });
    qsizetype count = cached.size();
    for (const auto& session : cached) {
        if (count <= 8 && bytes <= 16 * 1024 * 1024)
            break;
        if ((isActiveSession(session) && session->path == activePath_) || session->historyLoading ||
            session->copying || !session->pendingQuestionRequests.isEmpty())
            continue;
        for (const auto& entry : session->messages)
            for (auto it = entry.cbegin(); it != entry.cend(); ++it)
                if (it->metaType().id() == QMetaType::QString)
                    bytes -= it->toString().size() * 2;
        session->messages.clear();
        session->messageIndexByItem.clear();
        session->loadedIndexByItem.clear();
        session->loadedTurns.clear();
        session->historyLoaded = false;
        session->historyCursor.clear();
        session->historyHasNewer = false;
        session->historyNewerCursor.clear();
        session->historyTurnCursors.clear();
        --count;
    }
}

void AgentSessionManager::copyDaemonHistoryPage(const SessionPtr& session, int generation) {
    const auto cursor = session->copyCursor;
    const bool raw = server_->hasDaemonActivityGroups();
    auto params =
        cursor == "0"
            ? QJsonObject{}
            : transcript_paging::windowParams(
                  transcript_paging::Direction::Older, cursor,
                  raw ? session->copySnapshotCursor : session->historySnapshotCursor,
                  raw ? session->copyProjectionRevision : session->historyProjectionRevision);
    if (raw)
        params.insert("raw_activity_details", true);
    server_->readDaemonTranscriptWindow(
        session->threadId, params,
        [this, session, generation, cursor](const QJsonObject& page, const QString& failure) {
            if (!session->copying || session->copyGeneration != generation)
                return;
            QString error = failure;
            const auto next = error.isEmpty()
                                  ? nextCursor(page, cursor == "0" ? QString{} : cursor, error)
                                  : QString{};
            session->copySnapshotCursor = page.value("snapshot_cursor").toInteger();
            session->copyProjectionRevision = page.value("projection_revision").toInteger();
            if (!error.isEmpty()) {
                session->copying = false;
                ++session->copyGeneration;
                session->copyMessages.clear();
                session->copyTurns.clear();
                session->copyCursors.clear();
                emitHistoryState(session);
                emit errorOccurred(QStringLiteral("Could not copy conversation: %1").arg(error));
                return;
            }
            session->copyMessages =
                mergeDaemonHistory(daemonWindowMessages(page), session->copyMessages);
            session->copyCursor = next;
            copyHistoryPage(session, generation);
        });
}

} // namespace ketplus
