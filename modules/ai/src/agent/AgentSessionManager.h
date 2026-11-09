#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "agent/AgentTranscriptWindowBudget.h"

#include <functional>
#include <memory>

namespace ketplus {

class ActivityGroupLoader;
class CodexAppServer;
class CodexRateLimits;
class ProviderRuntimeStatus;
class ConversationWorktreeStore;

class AgentSessionManager final : public QObject {
    Q_OBJECT

  public:
    explicit AgentSessionManager(QObject* parent = nullptr);
    explicit AgentSessionManager(CodexAppServer* server, QObject* parent = nullptr);
    ~AgentSessionManager() override;

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] bool isRestoring() const noexcept { return restoring_; }
    CodexRateLimits* rateLimits() const { return rateLimits_; }
    ProviderRuntimeStatus* providerStatus() const { return providerStatus_; }
    CodexAppServer* appServer() const { return server_; }
    [[nodiscard]] QVariantList messages(const QString& worktreePath) const;
    [[nodiscard]] QVariant transcriptPayload(const QString& worktreePath, const QString& scope,
                                             const QString& messageKey, const QString& kind) const;
    [[nodiscard]] QVariantList conversations(const QString& worktreePath) const;
    [[nodiscard]] QVariantList archivedConversations() const;
    // ADR 0027: every listed conversation with a daemon number, latest activity first.
    [[nodiscard]] QVariantList numberedConversations() const;
    [[nodiscard]] QString title(const QString& worktreePath) const;
    [[nodiscard]] QString status(const QString& worktreePath) const;
    [[nodiscard]] bool isRunning(const QString& worktreePath) const;
    [[nodiscard]] QSet<QString> runningPaths() const;
    // Checkouts a running agent uses: each running session's execution worktree (canonical),
    // which can differ from the conversation path runningPaths() is keyed by, plus that path.
    [[nodiscard]] QSet<QString> runningCheckoutPaths() const;
    [[nodiscard]] QSet<QString> completedPaths() const;
    [[nodiscard]] QSet<QString> conversationPaths() const;
    [[nodiscard]] QString activeConversationId(const QString& worktreePath) const;
    [[nodiscard]] QVariantMap conversationConfiguration(const QString& worktreePath) const;
    [[nodiscard]] QVariantMap usageSnapshot(const QString& worktreePath) const;
    [[nodiscard]] QVariantList backgroundTasks(const QString& worktreePath) const;
    void heldMessageAction(const QString& worktreePath, const QString& messageId, bool release);
    // ADR 0067. readReviewer only reads; setReviewer assigns conversation #reviewerNumber,
    // starts a new reviewer with newProvider, or clears the reviewer when both are empty.
    void readReviewer(const QString& worktreePath);
    void setReviewer(const QString& worktreePath, const QString& reviewerNumber,
                     const QString& newProvider);
    void stopBackgroundTask(const QString& worktreePath, const QString& taskId);
    void readBackgroundTaskOutput(const QString& worktreePath, const QString& taskId);

    void executionWorktreeCommand(const QString& path, const QString& type,
                                  const QJsonObject& payload,
                                  std::function<void(const QJsonObject&, const QString&)> callback);
    void setModelOverride(const QString& model, const QString& effort = {});
    [[nodiscard]] QVariantList models() const { return models_; }
    void loadModels(const QString& cursor = {});
    // With a conversation id, that conversation becomes the worktree's current one before
    // anything is published, so opening it from another worktree resets the transcript once.
    void activate(const QString& worktreePath, bool loadTranscript = true,
                  const QString& conversationId = {});
    void submitPrompt(const QString& worktreePath, const QString& prompt,
                      const QVariantMap& options = {}, const QVariantList& attachments = {});
    void cancel(const QString& worktreePath);
    // `prompt` goes to the provider; the transcript shows `displayText` when the composer
    // wrapped what the user typed (skills, file references).
    void steerPrompt(const QString& worktreePath, const QString& prompt,
                     const QVariantList& attachments = {}, const QString& displayText = {});
    void selectConversation(const QString& worktreePath, const QString& conversationId);
    void markConversationSeen(const QString& worktreePath, const QString& conversationId);
    void renameConversation(const QString& worktreePath, const QString& conversationId,
                            const QString& title);
    bool archiveConversation(const QString& worktreePath, const QString& conversationId);
    bool restoreConversation(const QString& worktreePath, const QString& conversationId);
    bool cancelConversation(const QString& worktreePath, const QString& conversationId);
    bool deleteConversation(const QString& worktreePath, const QString& conversationId);
    // Copies an idle conversation into a new one in the same worktree and selects it. Answers
    // with conversationForkFinished.
    void forkConversation(const QString& worktreePath, const QString& conversationId);
    bool cancelWorktree(const QString& worktreePath);
    void archiveConversationsForWorktree(const QString& worktreePath);
    void setConversationConfiguration(const QString& worktreePath, const QString& model,
                                      const QString& effort, const QString& permission = {},
                                      const QString& provider = {},
                                      const QVariantMap& changes = {});
    void newSession(const QString& worktreePath);
    void markActiveConversationOwnsWorktree(const QString& worktreePath);
    [[nodiscard]] bool conversationOwnsWorktree(const QString& worktreePath,
                                                const QString& conversationId) const;
    [[nodiscard]] QStringList conversationOwnedWorktreePaths(const QString& worktreePath,
                                                             const QString& conversationId) const;
    [[nodiscard]] QVariantList ownedWorktrees(const QString& worktreePath) const;
    // The checkout the conversation's newest commands and edits ran in, when it is not
    // `worktreePath` itself; empty when the transcript names none that still exists.
    [[nodiscard]] QString recentWorkingCheckout(const QString& worktreePath) const;
    void hideOwnedWorktree(const QString& worktreePath, const QString& conversationId,
                           const QString& ownedPath);
    [[nodiscard]] QVariantMap historyState(const QString& worktreePath) const;
    // requestToken (> 0) is echoed as historyState.requestToken once the request completes,
    // fails, or is declined, so the view never completes a request on an unrelated update.
    // True while the reader is scrolling the active transcript. History ready to
    // be inserted waits until it turns false.
    void setTranscriptInputActive(bool active);
    // True from the first scroll input until the transcript comes to rest, near
    // the edges of the loaded window too. readingKey is the row being read when
    // it comes to rest; the window is trimmed around it shortly afterwards.
    void setTranscriptScrolling(bool scrolling, const QString& readingKey = {});
    // The row being read, reported as it changes rather than only when a scroll
    // comes to rest: a jump, find or keyboard navigation moves the reader
    // without scrolling. Empty while following the tail.
    void setTranscriptReadingKey(const QString& readingKey);
    // How tall the active transcript renders its loaded messages; sizes the
    // window that later history pages are trimmed to.
    void setTranscriptExtent(double renderedHeight, double viewportHeight);
    void loadOlderMessages(const QString& worktreePath, double viewportHeight = 0,
                           double averageRowHeight = 0, qint64 requestToken = 0);
    // Appends the next newer page (after_cursor) of a trimmed historical window.
    void loadNewerMessages(const QString& worktreePath, double viewportHeight = 0,
                           double averageRowHeight = 0, qint64 requestToken = 0);
    void loadLatestMessages(const QString& worktreePath);
    void copyConversation(const QString& worktreePath);
    void cancelConversationCopy(const QString& worktreePath);
    void setActivityGroupOpen(const QString& path, const QString& scope, const QString& key,
                              bool expanded);
    void answerQuestion(const QString& worktreePath, const QString& messageKey,
                        const QVariantMap& answers);

  signals:
    void executionContextChanged(const QString& path, const QString& conversation,
                                 const QString& branch, const QString& executionPath,
                                 qint64 revision);
    void modelsLoaded(const QVariantList& models);
    void messagesReset(const QString& worktreePath, const QVariantList& messages);
    void messagesPrepended(const QString& worktreePath, const QVariantList& messages);
    void messagesWindowReset(const QString& worktreePath, const QVariantList& messages);
    void historyStateChanged(const QString& worktreePath, const QVariantMap& state);
    void conversationCopyReady(const QString& worktreePath, const QString& conversationId,
                               const QString& text);
    void messageAppended(const QString& worktreePath, int index, const QVariantMap& message);
    void messageUpdated(const QString& worktreePath, int index, const QVariantMap& message);
    void sessionStateChanged(const QString& worktreePath, const QString& title,
                             const QString& status, bool running);
    void conversationsChanged(const QString& worktreePath, const QVariantList& conversations);
    void conversationConfigurationChanged(const QString& worktreePath,
                                          const QVariantMap& configuration);
    void conversationConfigurationApplyFinished(const QString& path, const QString& conversationId,
                                                const QString& error);
    void usageSnapshotChanged(const QString& worktreePath, const QVariantMap& snapshot);
    void backgroundTasksChanged(const QString& worktreePath, const QVariantList& tasks);
    // result: {output,total_bytes,truncated}, or {error} when the task cannot be read or stopped.
    void heldMessageResult(const QString& worktreePath, const QString& conversationId,
                           const QString& messageId, const QVariantMap& result);
    // result: {available, reviewer: {number,title,provider,model} or absent}, or {error, change}
    // where change tells a failed assignment from a failed read.
    void reviewerResult(const QString& worktreePath, const QString& conversationId,
                        const QVariantMap& result);
    void backgroundTaskResult(const QString& worktreePath, const QString& taskId,
                              const QString& action, const QVariantMap& result);
    void activityChanged();
    void ownedWorktreesChanged(const QString& worktreePath);
    void errorOccurred(const QString& message);
    void conversationDeletionFinished(const QString& worktreePath, const QString& conversationId,
                                      const QString& error);
    // forkId is empty when the fork failed; error then says why.
    void conversationForkFinished(const QString& worktreePath, const QString& conversationId,
                                  const QString& forkId, const QString& error);
    void steeringFinished(const QString& worktreePath, const QString& conversationId,
                          const QString& prompt, bool accepted);
    void restoreStateChanged(bool restoring);

  private:
    struct Session;
    using SessionPtr = std::shared_ptr<Session>;
    void forgetDeletedConversation(const SessionPtr& target);

    [[nodiscard]] SessionPtr ensureSession(const QString& worktreePath);
    // The composer's attachments as provider input after `text`: images by path (and as file
    // URLs for the transcript), text files folded into the text. Returns why one cannot be sent.
    [[nodiscard]] static QString attachmentInput(const QVariantList& attachments,
                                                 const QString& text, QJsonArray& input,
                                                 QStringList& imageSources);
    [[nodiscard]] SessionPtr sessionForThread(const QString& threadId) const;
    [[nodiscard]] bool isForkable(const SessionPtr& session) const;
    void ensureThread(const SessionPtr& session);
    void startTurn(const SessionPtr& session);
    void cancelSession(const SessionPtr& session);
    ActivityGroupLoader* activityGroups_{nullptr};
    void settleActivityGroups(const SessionPtr& session);
    void acceptActivityGroup(const SessionPtr& session, const QJsonObject& params);
    void publishActivityGroupDetails(const QString& session, const QString& group);
    void handleNotification(const QString& method, const QJsonObject& params);
    void acceptDaemonConfiguration(const SessionPtr& session, const QJsonObject& settings);
    void handleDaemonSession(const QJsonObject& stored);
    void handleUserInputRequest(const QJsonValue& requestId, const QJsonObject& params);
    void handleItem(const SessionPtr& session, const QJsonObject& item, bool completed);
    void hydrateHistory(const SessionPtr& session, const QJsonObject& thread);
    void hydrateConversation(const SessionPtr& session, bool loadOlder = false);
    void loadHistoryPage(const SessionPtr& session);
    void loadDaemonHistoryPage(const SessionPtr& session, bool newer = false,
                               int turnLimitOverride = 0);
    void scheduleLatestBackfill(const SessionPtr& session);
    void catchUpDaemonHistory(const SessionPtr& session);
    void cancelDaemonHistoryCatchup(const SessionPtr& session);
    void failDaemonHistoryCatchup(const SessionPtr& session, const QJsonObject& page,
                                  const QString& error);
    void continueDaemonHistoryCatchup(const SessionPtr& session, const QJsonObject& page);
    void loadLatestWindow(const SessionPtr& session, bool force = false);
    // Shows a conversation selected from another one: its retained window when that
    // is still current, otherwise a fresh newest window.
    void reopenConversation(const SessionPtr& session);
    struct DaemonHistoryPageSlot final {
        QJsonObject page;
        QVariantList projected;
        QString cursor;
        qint64 snapshotCursor{0};
        qint64 projectionRevision{0};
        qsizetype bytes{0};
        bool cached{false};
        bool done{false};
    };
    struct DaemonHistoryBatch final {
        quint64 epoch{0};
        bool initial{false};
        bool newer{false};
        int turnLimit{0};
        int pagesWanted{1};
        int pages{0};
        // One entry per page read, in reading order. A page is requested as soon
        // as the one before it arrives, so several are built at once; these keep
        // them in order until they are merged.
        QVector<DaemonHistoryPageSlot> readPages;
        // Pages still being built, and whether a page request is in flight.
        int projecting{0};
        bool fetching{false};
        // Set once the pages read are known to be the last of the batch.
        bool ended{false};
        QVariantList messages;
        QJsonObject firstPage;
        QJsonObject lastPage;
        QString next;
        // Layout trace: where a lazy load spends its time.
        QElapsedTimer clock;
        int cachedPages{0};
        qint64 readMs{0};
        qint64 measuredMs{0};
        qint64 preparedMs{0};
        qint64 idleMs{0};
        // The session's state serial when the first page was read.
        quint64 stateSerial{0};
    };
    using HistoryPageReceiver =
        std::function<void(const QJsonObject&, const QString&, bool, const QVariantList&)>;
    void refenceDaemonHistory(const SessionPtr& session, bool newer);
    void readDaemonHistoryBatchPageUncached(const SessionPtr& session,
                                            const std::shared_ptr<DaemonHistoryBatch>& batch,
                                            const QString& cursor, qint64 snapshotCursor,
                                            qint64 projectionRevision,
                                            const HistoryPageReceiver& received);
    static void releasePageWaiters(Session& session, bool newer, bool cached);
    void readDaemonHistoryBatchPage(const SessionPtr& session,
                                    const std::shared_ptr<DaemonHistoryBatch>& batch,
                                    const QString& cursor, qint64 snapshotCursor,
                                    qint64 projectionRevision);
    void finishDaemonHistoryBatch(const SessionPtr& session,
                                  const std::shared_ptr<DaemonHistoryBatch>& batch);
    void advanceDaemonHistoryBatch(const SessionPtr& session,
                                   const std::shared_ptr<DaemonHistoryBatch>& batch);
    // A batch merged into the session's window, computed off the GUI thread
    // against `basis`, a snapshot of the session's messages.
    struct PreparedDaemonWindow final {
        QString readingKey;
        QVector<QVariantMap> basis;
        QVector<QVariantMap> bounded;
        QVector<QVariantMap> untrimmed;
        transcript_window::TrimResult trim;
        QVariantList projected;
        QSet<QString> retainedTurns;
        QVariantList prefix;
        bool appended{false};
        QList<QPair<int, QVariantMap>> updates;
        // Outstanding requests the journal shows were already decided or expired.
        QSet<QString> settledPending;
    };
    static PreparedDaemonWindow
    prepareDaemonWindow(const QVariantList& batchMessages, const QVector<QVariantMap>& basis,
                        const QSet<QString>& pendingQuestionKeys, bool initial, bool newer,
                        const QHash<QString, qint64>& liveRowCursors, qint64 snapshotCursor,
                        int messageLimit, qsizetype byteLimit, const QString& readingKey);
    void prepareDaemonHistoryBatch(const SessionPtr& session,
                                   const std::shared_ptr<DaemonHistoryBatch>& batch);
    void applyDaemonHistoryBatch(const SessionPtr& session, const DaemonHistoryBatch& batch,
                                 const PreparedDaemonWindow& prepared);
    void whenTranscriptIdle(std::function<void()> run);
    void runIdleWaiters();
    bool transcriptInputActive_{false};
    bool transcriptScrolling_{false};
    QTimer historyTrimSettle_;
    [[nodiscard]] int historyWindowLimit(const Session& session) const;
    [[nodiscard]] qsizetype historyWindowByteLimit() const;
    [[nodiscard]] int historyBatchLimit(const Session& session,
                                        const DaemonHistoryBatch& batch) const;
    [[nodiscard]] qsizetype historyBatchByteLimit(const Session& session,
                                                  const DaemonHistoryBatch& batch) const;
    // Trims a window grown while scrolling back to its budget, centred on the
    // reader's row. Emits the new window only for the conversation on screen.
    void trimDaemonHistoryWindow(const SessionPtr& session, bool emitWindow);
    void installDaemonWindow(const SessionPtr& session, const QVector<QVariantMap>& bounded);
    void resumeNewerAfterTrim(const SessionPtr& session, const QVector<QVariantMap>& untrimmed,
                              qsizetype retained);
    void resumeOlderAfterTrim(const SessionPtr& session, const QVector<QVariantMap>& untrimmed,
                              const QVector<QVariantMap>& bounded);
    QList<std::function<void()>> idleWaiters_;
    QTimer idleSettle_;
    QTimer idleDeadline_;
    void pruneDaemonHistory();
    void prefetchDaemonHistory(const SessionPtr& session);
    void prefetchDaemonHistoryPage(const SessionPtr& session, bool newer);
    static void clearDaemonHistoryCache(Session& session);
    static void cacheDaemonHistoryPage(Session& session, bool newer, const QString& cursor,
                                       qint64 snapshotCursor, qint64 projectionRevision,
                                       const QJsonObject& page, const QVariantList& messages,
                                       qsizetype bytes);
    void copyDaemonHistoryPage(const SessionPtr& session, int generation);
    void emitHistoryState(const SessionPtr& session);
    void readHistoryPage(const SessionPtr& session, const QString& cursor,
                         std::function<void(QJsonArray, QString, QString)> callback);
    void copyHistoryPage(const SessionPtr& session, int generation);
    void emitConfiguration(const SessionPtr& session);
    void emitUsageSnapshot(const SessionPtr& session);
    void emitBackgroundTasks(const SessionPtr& session);
    void callBackgroundTask(const QString& worktreePath, const QString& taskId,
                            const QString& action);
    int appendMessage(const SessionPtr& session, QVariantMap message);
    [[nodiscard]] int messageIndexForItem(const SessionPtr& session, const QString& itemId);
    void updateMessage(const SessionPtr& session, int index, QVariantMap message);
    void appendAgentDelta(const SessionPtr& session, const QString& itemId, const QString& delta);
    void flushAgentDelta(const SessionPtr& session, const QString& itemId,
                         quint64 selectionGeneration = 0);
    void flushAgentDeltas(const SessionPtr& session);
    void publishDeferredFinal(const SessionPtr& session);
    void dismissPendingQuestions(const SessionPtr& session);
    void updateState(const SessionPtr& session, const QString& status, bool running);
    void failSession(const SessionPtr& session, const QString& error);
    void recordSessionError(const SessionPtr& session, const QString& error, bool notify);
    void syncConversationTitle(const SessionPtr& session);
    void persistWorkspace(const QString& path) const;
    void snapshotTurnWorktrees(const SessionPtr& session);
    void captureTurnWorktrees(const SessionPtr& session);
    // Calls `done(ok, paths)` once, asynchronously when git runs.
    void listRepositoryWorktrees(const QString& cwd,
                                 std::function<void(bool, const QStringList&)> done);
    [[nodiscard]] static bool turnMentionsPath(const SessionPtr& session, const QString& turnId,
                                               const QString& path);
    [[nodiscard]] static QString mainCheckoutPath(const QStringList& worktrees);
    void updateRestoreState(const SessionPtr& session);
    void selectSessionGeneration(const SessionPtr& session);
    bool markSeen(const SessionPtr& session);
    [[nodiscard]] bool isActiveSession(const SessionPtr& session) const;
    void applyModelOverride(QJsonObject& params, bool includeEffort) const;
    void callReviewer(const QString& worktreePath, const QString& method,
                      const QJsonObject& reviewer);
    // Reads that failed, often because the daemon had not listed its sessions yet. Each is
    // tried once more when the daemon finishes restoring.
    void retryReviewerReads();
    QSet<QString> reviewerReadsToRetry_;
    [[nodiscard]] static QString normalizedPath(const QString& path);

    CodexAppServer* server_{nullptr};
    CodexRateLimits* rateLimits_{nullptr};
    ProviderRuntimeStatus* providerStatus_{nullptr};
    std::unique_ptr<ConversationWorktreeStore> worktreeStore_;
    QHash<QString, SessionPtr> sessions_;
    QHash<QString, QVector<SessionPtr>> conversationsByPath_;
    QHash<QString, SessionPtr> sessionsByThread_;
    QHash<QString, QSet<QString>> deletedThreadsByPath_;
    QHash<QString, qint64> deletedDaemonGenerations_;
    QVariantList models_;
    bool loadingModels_{false};
    QString modelOverride_;
    QString effortOverride_;
    QString activePath_;
    quint64 nextSelectionGeneration_{0};
    bool restoring_{false};
    quint64 historyUseCounter_{0};
};

} // namespace ketplus
