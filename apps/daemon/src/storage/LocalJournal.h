#pragma once

#include "storage/AgentMessageRecord.h"
#include "storage/JournalSpace.h"

#include <QDateTime>
#include <QHash>
#include <functional>
#include <optional>
#include <utility>

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

namespace ketplus::daemon {

struct StoredSession final {
    QString id;
    QString worktreePath;
    QString adapter;
    QString threadId;
    QString turnId;
    QString state;
    qint64 generation{1};
    qint64 lastCursor{0};
    qint64 currentTurnSequence{0};
    QString title;
    qint64 configurationRevision{1};
    QString model;
    QString effort;
    QString executionPolicy{QStringLiteral("ask")};
    QString mode{QStringLiteral("default")};
    // JSON object: provider id -> {"thread_id", "seen_cursor"} for providers this conversation
    // has used besides the current one (whose thread is threadId).
    QString providerThreads{QStringLiteral("{}")};
    // Desktop sidebar read marker. Unread completed turns when lastSeenCursor < lastCursor.
    qint64 lastSeenCursor{0};
    // UTC ISO time of the latest user.message, empty before the first turn. Desktop and phones
    // list conversations and repositories by it, so every client shows the same order.
    QString lastTurnStartedAt;
    // Independent of provider/process generation and filesystem content revisions.
    qint64 contextRevision{1};
    // Requested native auto-compaction window. Zero inherits provider configuration.
    qint64 compactWindow{0};
};

// One command a paired device sent this Mac through the relay that could change something
// (#86): who sent it, what it was and how it ended. The relay keeps its own record; this one
// stays on the Mac.
struct RemoteCommandAudit final {
    QString commandId;
    QString kind;
    // As the command's content names it; the result code says whether its signature was
    // verified against a pinned device.
    QString deviceId;
    int deviceKeyVersion{0};
    QString sessionId;
    QString resultCode;
    QDateTime recordedAt;
};

struct SessionHead final {
    qint64 generation{0};
    qint64 cursor{0};
};

struct StagedAttachment final {
    QString id;
    QString name;
    QString mediaType;
    QByteArray bytes;
    qint64 revision{1};
    qint64 totalBytes{0};
    int chunkCount{0};
    QString digestSha256;
    QString status;
};

struct SessionTombstone final {
    QString sessionId;
    qint64 generation{0};
    qint64 lifecycleRevision{0};
    QString operationId;
    QString deletedAt;
};

struct EventReplayPage final {
    QJsonArray events;
    qint64 nextCursor{0};
};

struct TranscriptPage final {
    QJsonArray events;
    qint64 snapshotCursor{0};
    qint64 earliestTurnSequence{0};
    bool hasMoreBefore{false};
    // When the byte/event budget cuts a page, the oldest events are dropped so the newest tail
    // survives; firstCursor is the oldest cursor kept, and older content in the page's turns
    // was cut (truncated == true).
    bool truncated{false};
    qint64 firstCursor{0};
};

// A bounded suffix, not a claim that the first turn/item is fully loaded.
struct TranscriptWindow final {
    QJsonArray events;
    QJsonArray partialItems;
    qint64 snapshotCursor{0};
    qint64 projectionRevision{0};
    qint64 beforeCursor{0};
    bool hasMoreBefore{false};
    // Last event cursor of the page (exclusive continuation for after_cursor paging) and
    // whether a newer projected event exists at or below snapshotCursor.
    qint64 afterCursor{0};
    bool hasMoreAfter{false};
    bool partialFirstTurn{false};
    QString errorCode;
    QString errorMessage;
};

struct LargeEventFragment final {
    QString transferId;
    QString eventType;
    QString itemId;
    QString digestSha256;
    QByteArray data;
    qint64 offset{0};
    qint64 totalBytes{0};
    qint64 revision{0};
    qint64 nextOffset{0};
    bool complete{false};
    QString errorCode;
    QString errorMessage;
};

class LocalJournal final {
  public:
    // 2: mcpToolCall metadata-only projection with sanitized argument summaries (ADR 0008).
    static constexpr int currentCloudProjectionPolicyVersion = 2;
    explicit LocalJournal(const QString& path);
    ~LocalJournal();

    LocalJournal(const LocalJournal&) = delete;
    LocalJournal& operator=(const LocalJournal&) = delete;

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QString databasePath() const { return database_.databaseName(); }

    bool upsertSession(const StoredSession& session);
    // Advances last_seen_cursor with MAX so stale callers cannot move it backwards.
    bool markSessionSeen(const QString& sessionId, qint64 generation, qint64 cursor);
    bool deleteSession(const QString& sessionId, qint64 generation, QString* error = nullptr);
    bool deleteSession(const QString& sessionId, qint64 generation, const QString& operationId,
                       qint64 lifecycleRevision, QString* error = nullptr,
                       const QJsonArray& providerTargets = {});
    [[nodiscard]] QJsonArray deletionWork(int limit = 64, bool pendingCloudOnly = false,
                                          qint64 afterRow = 0) const;
    [[nodiscard]] QJsonArray sessionTombstones() const;
    bool completeProviderCleanup(const QString& sessionId, qint64 generation,
                                 const QString& provider, const QString& threadId);
    bool acknowledgeCloudDeletion(const QString& sessionId, qint64 generation,
                                  const QString& operationId);
    // Stores `fork` as a new session holding a copy of the source generation's transcript, in
    // one transaction. Fills the fork's cursor counters from the source.
    bool forkSession(const QString& sourceId, qint64 sourceGeneration, StoredSession& fork,
                     QString* error = nullptr);
    [[nodiscard]] SessionTombstone sessionTombstone(const QString& sessionId,
                                                    qint64 generation) const;
    [[nodiscard]] QList<StoredSession> sessions() const;
    // The id of every session, or nullopt when they could not all be read. sessions() returns
    // an empty list on a failed read, which must not pass for every session being deleted.
    [[nodiscard]] std::optional<QSet<QString>> sessionIds() const;
    [[nodiscard]] SessionHead sessionHead(const QString& sessionId) const;
    [[nodiscard]] QStringList worktreePaths() const;
    // Each catalog path with the repository recorded for it, empty when none was recorded.
    [[nodiscard]] QHash<QString, QString> worktreeRepositories() const;
    bool rememberWorktreeRepository(const QString& worktreePath, const QString& repositoryPath);
    qint64 turnContextRevision(const QString& sessionId, qint64 generation,
                               const QString& turnId) const;
    bool recordWorktreeActivity(const StoredSession& session, const QString& path, qint64 cursor);
    QSet<QString> touchedWorktreePaths(const QString& sessionId, qint64 generation) const;
    bool rememberWorktree(const QString& path);
    bool forgetWorktree(const QString& path);
    QString conversationPath(const StoredSession& session) const;
    bool recordConversationPath(const StoredSession& session);
    bool recordSessionContext(const StoredSession& session);
    [[nodiscard]] QJsonArray sessionContexts(const QString& sessionId, qint64 generation) const;
    // Context, event, catalog and idempotency result commit in one transaction.
    [[nodiscard]] qint64 switchSessionWorktree(const StoredSession& previous, const QString& target,
                                               const QString& messageId, const QJsonObject& result,
                                               QString* error = nullptr);
    [[nodiscard]] qint64 appendEvent(const QString& sessionId, qint64 generation,
                                     const QString& type, const QJsonObject& payload,
                                     QString* error = nullptr);
    [[nodiscard]] QJsonArray eventsAfter(const QString& sessionId, qint64 generation, qint64 cursor,
                                         int limit = 500) const;
    // True when the newest turn.started has no turn.completed after it.
    [[nodiscard]] bool hasOpenTurn(const QString& sessionId, qint64 generation) const;
    [[nodiscard]] EventReplayPage compactedEventsAfter(const QString& sessionId, qint64 generation,
                                                       qint64 cursor, int limit) const;
    [[nodiscard]] TranscriptPage transcriptPage(const QString& sessionId, qint64 generation,
                                                qint64 beforeTurnSequence, int turnLimit,
                                                qint64 beforeCursor = 0) const;
    [[nodiscard]] TranscriptWindow transcriptWindow(const QString& sessionId, qint64 generation,
                                                    qint64 beforeCursor = 0,
                                                    qint64 snapshotCursor = -1,
                                                    qint64 projectionRevision = -1,
                                                    int turnLimit = 3, int eventLimit = 128) const;
    [[nodiscard]] TranscriptWindow restoreTranscriptWindow(const QString& sessionId,
                                                           qint64 generation, qint64 beforeCursor,
                                                           qint64 snapshotCursor, int turnLimit = 3,
                                                           int eventLimit = 128) const;
    // Pages forward from an exclusive event cursor inside a fenced snapshot/projection.
    [[nodiscard]] TranscriptWindow
    transcriptWindowAfter(const QString& sessionId, qint64 generation, qint64 afterCursor,
                          qint64 snapshotCursor, qint64 projectionRevision, int turnLimit = 3,
                          int eventLimit = 128) const;
    // Summary projection and bounded, revision-fenced on-demand group events.
    [[nodiscard]] QJsonObject activitySummaryEvent(const QJsonObject& event) const;
    [[nodiscard]] QJsonObject activityGroupPage(const QString& sessionId, qint64 generation,
                                                const QJsonObject& params) const;
    static constexpr int maximumTranscriptWindowTurns = 20;
    [[nodiscard]] LargeEventFragment largeEventFragment(const QString& sessionId, qint64 generation,
                                                        const QString& transferId, qint64 offset,
                                                        int maximumBytes = 128 * 1024) const;
    [[nodiscard]] qint64 earliestCursor(const QString& sessionId, qint64 generation) const;
    [[nodiscard]] qint64 earliestCompactedCursor(const QString& sessionId, qint64 generation) const;
    [[nodiscard]] bool containsMediaReference(const QString& sessionId, qint64 generation,
                                              const QString& reference) const;
    // The tail of one command's output (64 KiB, 40 lines), read on demand by a phone.
    [[nodiscard]] QJsonObject commandOutput(const QString& sessionId, qint64 generation,
                                            qint64 turnSequence, const QString& itemId) const;
    [[nodiscard]] QJsonObject commandResult(const QString& messageId) const;
    bool storeCommandResult(const QString& messageId, const QJsonObject& result);
    // For an operation whose outcome is recorded in two steps: a provisional result before
    // the side effect, then the real one once it is known.
    bool replaceCommandResult(const QString& messageId, const QJsonObject& result);
    // Records how a remote command ended. A redelivered command keeps the row of its first
    // outcome, so the record shows what was done once.
    bool recordRemoteCommand(const RemoteCommandAudit& entry);
    // Newest first, at most `limit`, only those of deviceId unless it is empty; nullopt on a
    // failed read, with errorString() set.
    [[nodiscard]] std::optional<QList<RemoteCommandAudit>> remoteCommands(const QString& deviceId,
                                                                          int limit) const;
    // The journal's pages (#128); nullopt on a failed read, with errorString() set.
    [[nodiscard]] std::optional<journal_space::Usage> space() const;
    // Returns up to `maximumPages` free pages to the disk when the journal is on incremental
    // vacuum and holds at least minimumReclaimPages of them, then checkpoints the log so the
    // file shrinks (again at the next call when a reader held that back): how many it returned,
    // nullopt on a failed write. A batch is one short write, so it can run between turns.
    static constexpr qint64 minimumReclaimPages = 256;
    std::optional<qint64> reclaimFreePages(qint64 maximumPages);
    // Scan at most maximumRows replay rows per idle pass, retaining the last usage sample
    // of each type in old, acknowledged completed turns outside the newest 20 turns.
    // Also trim eligible completed command output to UTF-8 head/tail with an explicit marker.
    // Returns changed rows, or nullopt on error. The scan resumes in the next call.
    std::optional<qint64> pruneReplayDetails(const QDateTime& olderThan, int maximumRows = 128);
    bool beginAttachment(const QString& sessionId, qint64 generation, const QString& attachmentId,
                         qint64 expectedRevision, const QString& name, const QString& mediaType,
                         qint64 totalBytes, int chunkCount, const QString& digestSha256,
                         QString* error = nullptr);
    bool appendAttachmentChunk(const QString& sessionId, qint64 generation,
                               const QString& attachmentId, qint64 expectedRevision, int chunkIndex,
                               const QByteArray& bytes, const QString& digestSha256,
                               QString* error = nullptr);
    bool commitAttachment(const QString& sessionId, qint64 generation, const QString& attachmentId,
                          qint64 expectedRevision, QString* error = nullptr);
    bool cancelAttachment(const QString& sessionId, qint64 generation, const QString& attachmentId,
                          qint64 expectedRevision, QString* error = nullptr);
    [[nodiscard]] StagedAttachment attachment(const QString& sessionId, qint64 generation,
                                              const QString& attachmentId) const;
    [[nodiscard]] QJsonObject cloudEvent(const QString& sessionId, qint64 generation,
                                         qint64 cursor) const;
    [[nodiscard]] int cloudEventProjectionPolicyVersion(const QString& sessionId, qint64 generation,
                                                        qint64 cursor) const;
    bool storeCloudEvent(const QString& sessionId, qint64 generation, qint64 cursor,
                         const QJsonObject& event,
                         int projectionPolicyVersion = currentCloudProjectionPolicyVersion);
    bool removeCloudEventsThrough(const QString& sessionId, qint64 generation, qint64 cursor);
    bool acknowledgeCursor(const QString& clientId, const QString& sessionId, qint64 generation,
                           qint64 cursor);
    // Bounded retention for tables without a session lifecycle: idempotency results, deletion
    // tombstones and large payloads no longer referenced by any retained event.
    static constexpr int commandResultRetentionDays = 7;
    static constexpr int maximumCommandResults = 10000;
    static constexpr int tombstoneRetentionDays = 90;
    static constexpr int remoteCommandRetentionDays = 90;
    static constexpr int maximumRemoteCommands = 10000;
    bool pruneRetention();
    // The latest background task snapshot of each session that reported one.
    bool storeBackgroundTasks(const QString& sessionId, qint64 generation, const QJsonArray& tasks);
    [[nodiscard]] QHash<QString, QJsonArray> backgroundTaskSnapshots() const;

    // ADR 0027: host-unique conversation numbers. A number is an alias of a session generation;
    // deletion releases it into a quarantine before it can name another conversation.
    static constexpr int conversationNumberQuarantineDays = 30;
    [[nodiscard]] QString conversationNumber(const QString& sessionId, qint64 generation) const;
    QString assignConversationNumber(const QString& sessionId, qint64 generation,
                                     QString* error = nullptr);
    [[nodiscard]] std::optional<std::pair<QString, qint64>>
    sessionForConversationNumber(const QString& number) const;
    // Session id -> number for every conversation that holds one.
    [[nodiscard]] QHash<QString, QString> conversationNumbers() const;
    // ADR 0027: agent-to-agent messages.
    bool insertAgentMessage(const AgentMessageRecord& message, QString* error = nullptr);
    bool updateAgentMessage(const AgentMessageRecord& message, QString* error = nullptr);
    [[nodiscard]] std::optional<AgentMessageRecord> agentMessage(const QString& id) const;
    // Messages addressed to a conversation in the given states, oldest first.
    [[nodiscard]] QList<AgentMessageRecord>
    agentMessagesTo(const QString& sessionId, qint64 generation, const QStringList& states) const;
    // The latest messages a conversation sent or received, newest first.
    [[nodiscard]] QList<AgentMessageRecord> agentMessagesOf(const QString& sessionId,
                                                            qint64 generation, int limit) const;
    // Turns started automatically by messages of one chain, or for one receiver since a time.
    [[nodiscard]] int automaticAgentTurnsInChain(const QString& chainId) const;
    [[nodiscard]] int automaticAgentTurnsSince(const QString& sessionId, qint64 generation,
                                               const QDateTime& since) const;
    [[nodiscard]] QList<AgentMessageRecord> overdueAgentRequests(const QDateTime& now) const;
    [[nodiscard]] QStringList sessionsWithQueuedAgentMessages() const;

  private:
    bool initializeAgents();
    bool initializeRemoteAudit();
    // The audit's 90-day, 10,000-row bound. Run from pruneRetention and every 256 audit rows,
    // because commands refused before they ran store no command result to trigger it.
    bool pruneRemoteAudit();
    bool checkpointReclaimedPages();
    // Inside deleteSession's transaction: releases the number and ends undelivered messages.
    bool releaseAgentState(const QString& sessionId, qint64 generation, QString* error);
    bool writeCommandResult(const QString& messageId, const QJsonObject& result, bool replace);
    [[nodiscard]] qint64 appendEventWithMutation(const QString& sessionId, qint64 generation,
                                                 const QString& type, const QJsonObject& payload,
                                                 QString* error,
                                                 const std::function<bool()>& mutation);
    bool initializeContexts();
    [[nodiscard]] TranscriptWindow transcriptWindowImpl(const QString& sessionId, qint64 generation,
                                                        qint64 beforeCursor, qint64 snapshotCursor,
                                                        qint64 projectionRevision, int turnLimit,
                                                        int eventLimit, bool bindCurrentProjection,
                                                        qint64 afterCursor = 0) const;
    bool initialize();
    bool migrateTranscriptSchema();
    bool backfillTurnSequences();
    bool backfillReplayProjection();
    bool purgeExpiredAttachments();
    bool updateReplayProjection(const QString& sessionId, qint64 generation, qint64 cursor,
                                qint64 turnSequence, const QString& type,
                                const QString& encodedPayload, const QString& sentAt,
                                QString* error = nullptr);
    [[nodiscard]] qint64 appendLargeEvent(const QString& sessionId, qint64 generation,
                                          qint64 firstCursor, qint64 turnSequence,
                                          const QString& type, const QJsonObject& payload,
                                          const QByteArray& encodedPayload,
                                          QString* error = nullptr);
    bool pruneUnreferencedLargePayloads(const QString& sessionId, qint64 generation,
                                        const QStringList& transferIds);
    bool pruneOrphanLargePayloads();
    bool initializeActivityGroups();
    bool updateActivityGroups(const QString& sessionId, qint64 generation, qint64 cursor,
                              qint64 turnSequence, const QString& type, const QJsonObject& payload,
                              QString* error);
    void setError(const QString& error) const;

    QString connectionName_;
    QSqlDatabase database_;
    mutable QString error_;
    int commandResultsSincePrune_{0};
    int remoteCommandsSincePrune_{0};
    // A reclaim whose checkpoint a reader held back: the file has not shrunk yet.
    bool reclaimCheckpointPending_{false};
    qint64 detailRetentionScanRow_{0};
};

} // namespace ketplus::daemon
