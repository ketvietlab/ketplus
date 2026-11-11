#pragma once

#include "agent/AgentSessionManager.h"
#include <functional>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QStringList>
#include <QUuid>

namespace ketplus {

struct AgentSessionManager::Session final {
    QString id{QUuid::createUuid().toString(QUuid::WithoutBraces)};
    QString path;
    // Stable birth anchor remains path; navigation follows the acknowledged execution root.
    QString executionPath;
    QString executionBranch;
    qint64 executionRevision{0};
    QString threadId;
    QString turnId;
    QString title{QStringLiteral("New conversation")};
    // ADR 0027: the daemon's host-unique conversation number, such as "4821".
    QString number;
    // The conversation whose message started the current turn; empty for the user's turns.
    QString agentTurnNumber;
    QString agentTurnTitle;
    bool agentTurnReply{false};
    bool customTitle{false};
    // #125: the last turn ended asking the user to approve or decide ("Waiting for you").
    bool awaitingReply{false};
    bool archived{false};
    // When the daemon recorded the archive (`archived_at`); invalid until the catalog says.
    QDateTime archivedAt;
    // An archive or restore made here that the daemon has not confirmed (ADR 0100).
    bool archiveUnsent{false};
    bool archiveSyncPending{false};
    bool ownsWorktree{false};
    bool listed{false};
    QStringList turnWorktreeSnapshot;
    bool turnWorktreeSnapshotValid{false};
    QString turnWorktreeTurnId;
    // Bumped at each snapshot and capture, so a late `git worktree list` result is dropped.
    quint64 turnWorktreeGeneration{0};
    bool steeringPending{false};
    QString status{QStringLiteral("Ready")};
    // The latest reasoning heading of the running turn, for "Working · …" (#385).
    QString workingDetail;
    QString model;
    QString effort;
    qint64 configurationRevision{0};
    bool configurationApplying{false};
    QString permission{QStringLiteral("workspace-write")};
    qint64 contextUsedTokens{-1};
    qint64 contextLimitTokens{0};
    // Account plan windows, readable only while a turn's provider query is alive, so the
    // last reading stands in until the next turn. Rows: {id,label,usedPercent,resetsAt}.
    QVariantList planLimits;
    QVariantMap sessionUsage;
    // The daemon's report of the provider's background shells and agents; never persisted,
    // since a restarted provider no longer runs them.
    QVariantList backgroundTasks;
    QString pendingPrompt;
    // Pairs the locally shown prompt with its journaled user.message echo.
    QString pendingClientMessageId;
    QJsonArray pendingInput;
    QVariantMap options;
    QVector<QVariantMap> messages;
    QHash<QString, int> messageIndexByItem;
    // Every itemId row loaded from a daemon window, across all turns. Lets a late update
    // for an earlier turn edit its row even when the event arrives without that turn scope.
    QHash<QString, int> loadedIndexByItem;
    QHash<QString, QString> agentMessagePhaseByItem;
    QHash<QString, QString> deferredAgentTextByItem;
    QStringList deferredAgentOrder;
    QHash<QString, QString> pendingFinalDeltas;
    QSet<QString> scheduledFinalDeltaFlushes;
    bool explicitFinalInTurn{false};
    QHash<QString, QJsonValue> pendingQuestionRequests;
    // Attention metadata also follows unloaded background conversations, without their cards.
    QSet<QString> pendingInputIds;
    bool running{false};
    bool completed{false};
    qint64 lastEventCursor{0};
    // When the latest turn started (ms since epoch, 0 = never). Orders the sidebar, which
    // merges a repository's worktrees into one list. With a daemon this is the daemon's
    // user.message time, the same one phones order by.
    qint64 lastTurnStartedAt{0};
    qint64 lastSeenCursor{0};
    bool readyInProcess{false};
    bool cancelRequested{false};
    bool pendingCompact{false};
    QSet<QString> activeCompactions;
    bool attaching{false};
    bool historyLoaded{false};
    bool historyDirty{false};
    bool historyCatchingUp{false};
    bool historyLoading{false};
    // A cached conversation is refreshing its newest page; sending may proceed
    // while the daemon validates the replacement window.
    bool historyRefreshingLatest{false};
    // A fresh newest-turn page is on screen; fetch the preceding turns next.
    bool historyLatestBackfillPending{false};
    quint64 selectionGeneration{0};
    quint64 historyRequestGeneration{0};
    quint64 transcriptGeneration{1};
    QString historyCursor;
    QString historyError;
    // Which paging direction historyError came from, so the retry control asks
    // the daemon for the page that actually failed. Window-level failures
    // (replay, approval sync) record false and retry as an older page.
    bool historyErrorNewer{false};
    bool historyResyncRequired{false};
    bool historyHasNewer{false};
    // Exclusive after_cursor for the next newer page; empty when newer rows cannot be paged.
    QString historyNewerCursor;
    // Token of the latest transcript page request (older or newer), echoed in historyState so
    // the view completes exactly that request; 0 when the caller supplied none.
    qint64 historyRequestToken{0};
    // Durable [first, last] event cursor per loaded daemon turn id, for window-edge continuation.
    QHash<QString, QPair<qint64, qint64>> historyTurnCursors;
    // Live rows newer than a transcript page's snapshot must survive its
    // asynchronous projection and installation.
    qint64 daemonLiveEventCursor{0};
    QHash<QString, qint64> daemonLiveRowCursors;
    QHash<QString, QVector<QPair<qint64, QString>>> daemonDeferredDeltasByItem;
    // Latest viewport geometry hint from the transcript (0 = unknown).
    double historyViewportHeight{0};
    double historyAverageRowHeight{0};
    // Rendered transcript height per loaded message. Unlike the average visible
    // row, it counts messages folded into activity groups, so it predicts how
    // much height a page of messages adds.
    double historyHeightPerMessage{0};
    // Layout trace: timings of the last lazy load that reached the window.
    QVariantMap historyLastBatch;
    QSet<QString> loadedTurns;
    qint64 historySnapshotCursor{0};
    qint64 historyProjectionRevision{0};
    quint64 historyEpoch{0};
    // Daemon pages read ahead of the loaded window in both directions, keyed by
    // direction, cursor and snapshot fence, so a lazy load finds its pages in
    // memory instead of waiting on the daemon.
    struct CachedHistoryPage final {
        QJsonObject page;
        QVariantList messages;
        qsizetype bytes{0};
    };
    QHash<QString, CachedHistoryPage> historyPageCache;
    qsizetype historyPageCacheBytes{0};
    qint64 historyPageCacheSnapshot{0};
    qint64 historyPageCacheRevision{0};
    quint64 historyPrefetchGeneration{0};
    bool historyPrefetchingOlder{false};
    bool historyPrefetchingNewer{false};
    // Cache key of the page each read-ahead is fetching. A lazy load that needs
    // that page waits for it instead of asking the daemon a second time.
    QString historyPrefetchKeyOlder;
    QString historyPrefetchKeyNewer;
    QHash<QString, QVector<std::function<void(bool)>>> historyPageWaiters;
    // Direction of the lazy load in progress; read-ahead the other way pauses.
    bool historyLoadingNewer{false};
    // Messages the loaded window may hold, from how tall the view renders them.
    int historyMessageBudget{300};
    // The row the reader was on when scrolling last stopped; an idle trim keeps
    // the window centred on it.
    QString historyReadingKey;
    // Consecutive snapshot_changed failures while paging a loaded window.
    int historySnapshotRetries{0};
    // Bumped by every live state update, so a history window read before one can
    // tell that its state is older than what the session already shows.
    quint64 stateSerial{0};
    quint64 historyLastUsed{0};
    qint64 daemonTurnSequence{0};
    qint64 daemonGeneration{0};
    // The agent the daemon last reported running this conversation; empty until it reports.
    QString daemonAdapter;
    bool copying{false};
    int copyGeneration{0};
    QString copyCursor;
    qint64 copySnapshotCursor{0}, copyProjectionRevision{0};
    QVariantList copyMessages;
    QSet<QString> copyTurns;
    QSet<QString> copyCursors;
};

// A daemon journal timestamp (UTC ISO 8601 with milliseconds) as ms since epoch, 0 if invalid.
inline qint64 daemonTimestamp(const QString& value) {
    const QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
    return time.isValid() ? time.toMSecsSinceEpoch() : 0;
}

} // namespace ketplus
