#pragma once

#include "agents/AgentBroker.h"
#include "agents/AgentToolLaunch.h"
#include "runtime/JournalStorageJobs.h"
#include "runtime/ProviderCatalogCache.h"
#include "runtime/ProviderReadiness.h"
#include "storage/LocalJournal.h"
#include "storage/McpMediaStore.h"

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>

#include <functional>
#include <memory>

namespace ketplus::daemon {

class ApprovalBroker;
class ClaudeAdapter;
class ClaudeAccountUsage;
class CodexAdapter;
class CursorAdapter;
class ProviderAdapter;

class SessionHost final : public QObject, private AgentHostPort {
    Q_OBJECT

  public:
    explicit SessionHost(LocalJournal* journal, CodexAdapter* adapter, QObject* parent = nullptr);
    SessionHost(LocalJournal* journal, CodexAdapter* adapter, ClaudeAdapter* claude,
                QObject* parent = nullptr);
    SessionHost(LocalJournal* journal, CodexAdapter* adapter, ClaudeAdapter* claude,
                CursorAdapter* cursor, QObject* parent = nullptr);
    ~SessionHost() override;

    void setClaudeAccountUsage(ClaudeAccountUsage* usage) { claudeAccountUsage_ = usage; }

    using CommandCallback = std::function<void(const QJsonObject&)>;

    [[nodiscard]] QJsonObject handleCommand(const QJsonObject& request, const QString& clientId);
    // As handleCommand, except that session.steer answers only once the provider has taken or
    // refused the text. A phone clears its draft on success, so "accepted" before the provider
    // answers lost the text whenever the provider then refused it. A steer whose provider never
    // answers within steerOutcomeTimeoutMs, or whose daemon restarted before it answered, ends
    // as steer_outcome_unknown rather than being sent twice.
    void handleCommandDeferred(const QJsonObject& request, const QString& clientId,
                               CommandCallback done);
    static constexpr int steerOutcomeTimeoutMs = 20'000;
    // Streamed text is journaled at most this often per session: consecutive deltas of one item
    // become one event. One event per provider chunk (a word or less) made every reader, above
    // all the phone paging a relay, read tens of thousands of events per conversation.
    static constexpr int deltaCoalesceMs = 150;
    static constexpr qsizetype maximumCoalescedDeltaBytes = 16 * 1024;
    // Cloud restore requests bind the current local projection revision while
    // validating their advertised snapshot head in one journal read transaction.
    [[nodiscard]] QJsonObject restoreTranscriptWindow(const QJsonObject& request) const;
    [[nodiscard]] bool hasActiveSessions() const;
    // Deletes at most one conversation archived longer than the Storage option
    // `archived_history_days` (ADR 0108; Never keeps them), through the same guards as
    // session.delete. Returns whether one was deleted.
    bool expireArchivedSessions(const QDateTime& now);
    [[nodiscard]] QJsonObject configuration(const QJsonObject& request,
                                            const QJsonArray& providerModels) const;
    // The settings a conversation started from the phone begins with, and what it can choose
    // before its first message: the same shape as `configuration`, for a session not yet made.
    [[nodiscard]] QJsonObject newConversationConfiguration(const QJsonArray& providerModels) const;
    void handleProviderQuery(const QJsonObject& request,
                             std::function<void(const QJsonObject&, const QString&)> callback);
    // session.fork: the provider copies the session's native thread and the journal copies
    // its transcript into a new session. Answers once the provider has replied.
    void forkSession(const QJsonObject& request, std::function<void(const QJsonObject&)> callback);
    // session.task.stop and session.task.output: one background task of a Claude session, by
    // the task_id its latest tasks.changed event listed. Answers once the provider has replied.
    void backgroundTaskCommand(const QJsonObject& request, CommandCallback done);
    [[nodiscard]] static bool isBackgroundTaskCommand(const QString& type);
    // ADR 0015 capability of the provider behind `adapter` ("codex", "claude" or "cursor").
    [[nodiscard]] bool providerSupports(const QString& adapter, const QString& capability) const;
    // The provider for a session's adapter name, or nullptr when it is not installed.
    [[nodiscard]] ProviderAdapter* provider(const QString& adapter) const;
    [[nodiscard]] ProviderAdapter* availableProvider(const QString& adapter) const;
    [[nodiscard]] QString missingProviderMessage(const QString& adapter) const;
    // ADR 0027: the local socket and executable the injected ketplus_agents server uses. Until
    // set (tests that drive SessionHost directly), turns get no agent tools.
    void setAgentEndpoint(const QString& socketName, const QString& executable);
    [[nodiscard]] bool isAgentToken(const QString& token) const;
    // Conversations whose running turn another conversation's message started (ADR 0027).
    [[nodiscard]] QHash<QString, QJsonObject> agentTurnSenders() const;
    // agents.* operations of the conversation that holds `token`.
    [[nodiscard]] QJsonObject handleAgentCommand(const QString& token, const QString& type,
                                                 const QJsonObject& payload);
    [[nodiscard]] QString agentToken(const QString& sessionId);

  signals:
    void eventReady(const QJsonObject& event);

  private:
    ClaudeAccountUsage* claudeAccountUsage_{nullptr};
    void readClaudeAccountUsage(const QJsonObject& request, const QString& clientId,
                                CommandCallback done);
    struct Session final {
        StoredSession stored;
        QString pendingProviderTitle;
        bool attachedToProvider{false};
        qint64 providerCompactWindow{0};
        bool startingTurn{false};
        qint64 submittedContextRevision{0};
        QHash<QString, qint64> turnContexts;
        // A Stop can arrive while an adapter is still creating its provider-native session.
        // Keep it until that adapter has actually started the prompt.
        bool interruptPending{false};
        // A client's Stop is ending this turn, so its interrupted end says so.
        bool stopRequested{false};
        // The provider turn that last completed in the current provider process. A repeated
        // completion for it, or a late turn/started, must not end or restart the turn that runs
        // now. Cleared when the process stops or reloads, since the next one may reuse ids.
        QString completedTurnId;
        // The turn a final provider error ended (Codex `error` with willRetry false), which may
        // never send turn/completed. Its approvals are closed: a later request for it is declined.
        // Cleared with completedTurnId.
        QString failedTurnId;
        // SHA-256 digests of items whose completion is journaled or queued, oldest first and
        // bounded. A stream delta for one of them would be appended to final text. Cleared with
        // completedTurnId, since a new provider process may reuse item ids; an item.started
        // with the same id reopens it.
        QSet<QByteArray> completedItems;
        QList<QByteArray> completedItemOrder;
        QStringList materializedAttachments;
        QStringList stagedAttachmentIds;
        // The provider's latest background task snapshot (tasks.changed, REPLACE semantics).
        QJsonArray backgroundTasks;
        // Worktrees of the repository when a turn started without its own (deleted) checkout;
        // one the turn adds is where the conversation continues.
        QStringList worktreesBeforeTurn;
        // Explicit successful tool activity may select the next turn's default checkout.
        QString workingWorktree;
        qint64 worktreeFollowRevision{0};
        // Set when the stored thread belonged to another provider (an old journal): the next
        // turn starts a fresh thread pointed at the transcript of this provider's history.
        QString handoffFrom;
        // Set when the running turn called a KetPlus browser tool; its end closes the sessions.
        bool browserUsedThisTurn{false};
        // Last `provider.runtime` warning journaled for this session in this daemon.
        QString noticedRuntime;
        // `provider.protocol_error` rows already journaled, keyed by method and missing fields.
        QSet<QString> reportedProtocolErrors;
        // An assistant.delta or command.output_delta not journaled yet; its `delta` grows until
        // another event, another item, the size cap or the flush timer journals it.
        QString pendingDeltaType;
        QJsonObject pendingDelta;
        struct DeferredJournalEvent final {
            QString type;
            QJsonObject payload;
            qsizetype bytes{0};
        };
        QList<DeferredJournalEvent> deferredJournal;
        qsizetype deferredJournalBytes{0};
        QDeadlineTimer journalRetryAt{0};
        QString lastJournalError;
        bool journalBufferFullReported{false};
    };

    [[nodiscard]] QJsonObject listSessions(const QJsonObject& payload) const;
    [[nodiscard]] QJsonObject storageRetention(const QString& type, const QJsonObject& payload);
    [[nodiscard]] QJsonObject storageSize(const QString& type, const QJsonObject& payload);
    [[nodiscard]] QJsonObject storageOptions(const QString& type, const QJsonObject& payload);
    // Warning, over-limit and low-disk state for the sidebar and remote clients (ADR 0108).
    [[nodiscard]] QJsonObject storageStatus() const;
    static constexpr qint64 lowDiskBytes = 5'000'000'000LL;
    JournalStorageJobs& storageJobs();
    std::unique_ptr<JournalStorageJobs> storageJobs_;
    QJsonObject workspaceCatalog(const QString& type, const QJsonObject& payload);
    [[nodiscard]] QJsonObject eventsAfter(const QJsonObject& request) const;
    [[nodiscard]] QJsonObject transcriptPage(const QJsonObject& request) const;
    [[nodiscard]] QJsonObject activityGroupPage(const QJsonObject& request) const;

  public:
    [[nodiscard]] QJsonObject activitySummaryEvent(const QJsonObject& event) const {
        return journal_->activitySummaryEvent(event);
    }

  private:
    [[nodiscard]] QJsonObject transcriptWindow(const QJsonObject& request) const;
    [[nodiscard]] QJsonObject transcriptWindow(const QJsonObject& request,
                                               bool bindCurrentProjection) const;
    [[nodiscard]] QJsonObject transcriptLargeEvent(const QJsonObject& request) const;
    // session.output.read / session.media.read: bounded reads a phone asks for on demand.
    [[nodiscard]] QJsonObject ephemeralRead(const QJsonObject& request);
    // session.media.read for a caller that can wait: the preview is made off the main thread.
    void mediaReadDeferred(const QJsonObject& request, CommandCallback done);
    [[nodiscard]] QJsonObject pendingApprovals(const QJsonObject& request) const;
    // A local client may hand the provider images by path; a remote one (the phone) stages them
    // as attachments first, since it cannot see the Mac's files and must not name them.
    // `agentOrigin` (ADR 0027) marks a turn an agent message started.
    [[nodiscard]] QJsonObject startTurn(const QJsonObject& request, bool localClient = true,
                                        const QJsonObject& agentOrigin = {});
    void recordWorktreeActivity(Session& session, const QString& type, const QJsonObject& payload,
                                qint64 cursor);
    void processDeletionWork();
    // Drives bounded passes over deleted sessions' remaining history (ADR 0093).
    void scheduleHistoryPurge(int delayMs);
    void purgeDeletedHistory();
    bool historyPurgeScheduled_{false};
    // Drives bounded passes indexing activity behind older history (ADR 0097).
    void scheduleActivityBackfill(int delayMs);
    void backfillActivity();
    bool activityBackfillScheduled_{false};
    [[nodiscard]] std::optional<QJsonArray>
    providerCleanupTargets(const StoredSession& session) const;
    QSet<QString> providerCleanupInFlight_;
    qint64 deletionWorkCursor_{0};
    [[nodiscard]] QJsonObject resolveResource(const QJsonObject& request);
    [[nodiscard]] QString worktreeCatalogRoot(const Session& session) const;
    [[nodiscard]] QJsonObject sessionWorktrees(const QJsonObject& request);
    [[nodiscard]] QJsonObject switchWorktree(const QJsonObject& request);
    // Where a turn runs: the conversation's worktree, or after it was deleted the repository's
    // primary worktree (else the nearest surviving parent folder).
    [[nodiscard]] QString executionDirectory(const Session& session) const;
    void noteWorktreesBeforeTurn(Session& session);
    // Follows an explicitly used checkout, or a replacement of a deleted checkout, after a turn.
    void followRecreatedWorktree(Session& session);
    [[nodiscard]] QJsonObject moveSessionWorktree(Session& session, const QString& target,
                                                  const QString& messageId);
    [[nodiscard]] QJsonObject renameSession(const QJsonObject& request);
    [[nodiscard]] QJsonObject markSessionSeen(const QJsonObject& request);
    [[nodiscard]] QJsonObject setSessionArchived(const QJsonObject& request);
    [[nodiscard]] QJsonObject setSessionTags(const QJsonObject& request);
    [[nodiscard]] QJsonObject deleteSession(const QJsonObject& request);
    // With `outcome`, returns an empty object and answers through it once the provider has
    // replied; without it, answers "accepted" as soon as the text is handed to the provider.
    // Steering takes images the same way startTurn does.
    [[nodiscard]] QJsonObject steer(const QJsonObject& request, CommandCallback outcome = {},
                                    bool localClient = true, const QJsonObject& agentOrigin = {});
    [[nodiscard]] QJsonObject interrupt(const QJsonObject& request);
    // A turn.completed payload; it takes the session's pending Stop and carries the work report
    // the turn ended with (ADR 0106).
    [[nodiscard]] QJsonObject turnCompletion(Session& session, const QString& status);
    [[nodiscard]] QJsonObject resolveApproval(const QJsonObject& request);
    [[nodiscard]] QJsonObject applyConfiguration(const QJsonObject& request, bool localClient);
    [[nodiscard]] QJsonObject applyExecutionPolicy(const QJsonObject& request);
    [[nodiscard]] QJsonObject applyMode(const QJsonObject& request);
    [[nodiscard]] QJsonObject beginAttachment(const QJsonObject& request);
    [[nodiscard]] QJsonObject appendAttachmentChunk(const QJsonObject& request);
    [[nodiscard]] QJsonObject commitAttachment(const QJsonObject& request);
    [[nodiscard]] QJsonObject cancelAttachment(const QJsonObject& request);
    [[nodiscard]] QJsonObject acknowledge(const QJsonObject& request, const QString& clientId);
    [[nodiscard]] QJsonObject heldAgentMessages(const QJsonObject& request, bool release);
    // ADR 0067: the conversation's reviewer, read or replaced by a local client.
    [[nodiscard]] QJsonObject conversationReviewer(const QJsonObject& request, bool apply);
    [[nodiscard]] QJsonObject reviewerJson(const QString& sessionId) const;
    QString startReviewerConversation(const Session& working, const QString& provider,
                                      QString* error);
    // Copies the source history in bounded event-loop passes (ADR 0094), then answers.
    struct PendingFork;
    void storeFork(const QString& sourceId, qint64 sourceGeneration, const QString& forkId,
                   const QString& threadId, std::function<void(const QJsonObject&)> finish);
    void advanceFork(const std::shared_ptr<PendingFork>& pending);
    void completeFork(const std::shared_ptr<PendingFork>& pending);
    void abandonFork(const std::shared_ptr<PendingFork>& pending, const QJsonObject& failure);
    QSet<QString> forksInFlight_;
    [[nodiscard]] QJsonObject checkedSession(const QJsonObject& request, Session** session);
    [[nodiscard]] QJsonObject configurationOf(const StoredSession& stored,
                                              const QJsonArray& mergedCatalog) const;
    void prepareTurn(const QString& sessionId, qint64 generation, const QJsonArray& input,
                     const QJsonObject& options);
    // ADR 0015: installed provider CLI versions and their support status.
    void providerRuntimeStatus(std::function<void(const QJsonObject&, const QString&)> callback);
    // Codex readiness from account/read (#170); answers within codexAccountReadTimeoutMs.
    void codexReadiness(const compat::Runtime& runtime,
                        std::function<void(const QJsonObject&)> callback);
    static constexpr int codexAccountReadTimeoutMs = 5000;
    // Journals `provider.runtime` once when a session's CLI is untested or unreadable.
    void noteProviderRuntime(const QString& sessionId, qint64 generation);
    void beginTurn(const QString& sessionId, qint64 generation, const QJsonArray& input,
                   const QJsonObject& options);
    void beginClaudeTurn(const QString& sessionId, qint64 generation, const QJsonArray& input,
                         const QJsonObject& options);
    void beginCursorTurn(const QString& sessionId, qint64 generation, const QJsonArray& input,
                         const QJsonObject& options);
    void handleProviderNotification(const QString& method, const QJsonObject& params);
    // ADR 0015 §5: false when a Codex notification must not be handled. Methods KetPlus does
    // not act on are counted; known ones missing a required field journal
    // `provider.protocol_error` instead of being read as empty values.
    [[nodiscard]] bool acceptProviderNotification(Session* session, const QString& method,
                                                  const QJsonObject& params);
    void handleCanonicalEvent(const QString& provider, const QString& sessionId,
                              const QString& type, const QJsonObject& payload);
    void handleProviderRequest(const QJsonValue& requestId, const QString& method,
                               const QJsonObject& params);
    void handleCanonicalPermission(const QString& adapterName, const QJsonValue& requestId,
                                   const QJsonObject& params);
    void registerAssistantQuestions(Session& session, const QJsonObject& item);
    bool answerAssistantQuestions(const QJsonValue& id, const QJsonObject& result);
    QSet<QString> assistantQuestions_;
    void handleCanonicalQuestion(const QJsonValue& requestId, const QJsonObject& params,
                                 const QString& provider = QStringLiteral("cursor"));
    void routeProviderResponse(const QJsonValue& requestId, const QJsonObject& result);
    void routeProviderError(const QJsonValue& requestId, const QString& code,
                            const QString& message);
    // Answers a provider request KetPlus cannot handle with a refusal and journals
    // `provider.request_declined` so the transcript explains the refusal.
    void declineProviderRequest(Session& session, const QJsonValue& requestId,
                                const QString& method, const QString& reason,
                                const QString& serverName);
    // A turn ended: its pending approvals and questions expire without a provider answer.
    void abandonTurnApprovals(const QString& sessionId);
    // Drops the Codex routes of requests that were abandoned without an answer.
    void forgetProviderRoutes(const QList<QJsonValue>& requestIds);
    // A turn that never started: clears the start and any Stop pressed meanwhile, then fails.
    void failTurnStart(Session& session, const QString& message);
    void providerStopped(const QString& adapterName, const QString& reason);
    // Journals the snapshot as tasks.changed and keeps it for restarts and task commands.
    void recordBackgroundTasks(Session& session, const QJsonArray& tasks);
    static bool hasLiveBackgroundTasks(const Session& session);
    // The process that ran the session's background tasks is gone: whatever still ran ended.
    void endBackgroundTasks(Session& session, const QString& reason);
    void providerReloaded(const QString& reason);
    // Adds a message's images and text files to its provider input and records each image in
    // `messageImages`. Staged attachments and temporary copies join the turn's, released when
    // it ends; on failure only those this call added are dropped, and the error is returned.
    [[nodiscard]] QJsonObject attachMessageMedia(Session& session, const QJsonObject& payload,
                                                 bool localClient, QJsonArray& input,
                                                 QJsonArray& messageImages);
    void releaseTurnAttachments(Session& session);
    // What a provider that takes a single prompt reads from a message's input, and the paths of
    // its images.
    [[nodiscard]] static QString promptText(const QJsonArray& input);
    [[nodiscard]] static QJsonArray imagePaths(const QJsonArray& input);
    [[nodiscard]] bool updateSessionTitle(Session& session, const QString& title, bool customTitle);
    void syncProviderTitle(Session& session);
    void refreshProviderTitle(const QString& sessionId, qint64 generation);
    // Returns false (and logs) when the journal rejects the event; no eventReady is emitted.
    // Deltas are held back and coalesced (see deltaCoalesceMs); any other event first journals
    // the session's held delta so the journal keeps provider order.
    bool appendEvent(Session& session, const QString& type, const QJsonObject& payload,
                     QString* error = nullptr);
    bool journalTurnStart(Session& session, const QJsonObject& prompt,
                          const QJsonObject& configuration, QString* error);
    bool journalEvent(Session& session, const QString& type, const QJsonObject& payload,
                      QString* error = nullptr);
    bool flushDelta(Session& session, QString* error = nullptr);
    void flushDeltas();
    bool deferJournalEvent(Session& session, const QString& type, const QJsonObject& payload,
                           QString* error);
    qsizetype deferredJournalBytes_{0};
    qsizetype deferredJournalCount_{0};
    QTimer deltaFlush_;
    [[nodiscard]] Session* sessionForThread(const QString& threadId);
    void noteBrowserToolItem(Session& session, const QJsonObject& item);
    void closeTurnBrowserSessions(Session& session);
    [[nodiscard]] static bool isMutating(const QString& messageType);
    [[nodiscard]] static QString normalizedWorktree(const QString& path);

    // AgentHostPort (ADR 0027), in SessionHostAgents.cpp.
    [[nodiscard]] std::optional<AgentConversation>
    agentConversation(const QString& sessionId) const override;
    [[nodiscard]] QList<AgentConversation> agentConversations() const override;
    QString startAgentTurn(const QString& sessionId, qint64 generation, const QString& providerText,
                           const QJsonObject& origin) override;
    QString steerAgentTurn(const QString& sessionId, qint64 generation, const QString& providerText,
                           const QJsonObject& origin) override;
    [[nodiscard]] qint64 agentTurnSequence(const QString& sessionId) const override;
    void appendAgentNotice(const QString& sessionId, qint64 generation,
                           const QJsonObject& payload) override;
    // ADR 0106, in SessionHostWorkReports.cpp.
    QJsonObject recordWorkReport(const QString& sessionId, qint64 generation,
                                 const QJsonObject& payload) override;
    [[nodiscard]] QJsonObject readWorkReport(const QJsonObject& request);
    [[nodiscard]] AgentConversation agentConversationOf(const Session& session) const;
    // The ketplus_agents launch for a conversation generation, when an endpoint is set.
    [[nodiscard]] std::optional<agent_tools::Launch> agentLaunch(const QString& sessionId,
                                                                 qint64 generation);
    // Provider instructions naming the conversation's number; empty without an endpoint.
    [[nodiscard]] QString agentInstructions(const Session& session) const;
    void addCodexAgentTools(Session& session, QJsonObject& params, QJsonObject config);
    // Prepends queued agent messages and appends `#NNNN` references to a user turn's input.
    void addAgentContext(Session& session, const QString& userText, QJsonArray& input,
                         bool userTurn);

    ProviderCatalogCache catalogCache_;
    LocalJournal* journal_{nullptr};
    // Codex is also reached through `adapter_` for what only it has: app-server notifications,
    // thread listing and the browser-settings reload.
    CodexAdapter* adapter_{nullptr};
    // Every installed provider by adapter name, Codex included; the order is the merge order
    // of model lists and catalogs.
    QList<ProviderAdapter*> providers_;
    ApprovalBroker* approvals_{nullptr};
    QHash<QString, Session> sessions_;
    QHash<QString, QString> sessionsByThread_;
    QHash<QString, QString> pendingProviderByRequest_;
    // Codex notifications ignored since the daemon started, per method (diagnostics only).
    QHash<QString, qint64> ignoredNotifications_;
    // Where a conversation's handoff transcript is written; readable by the providers it hands
    // over to.
    [[nodiscard]] QString handoffDirectory(const QString& sessionId) const;
    QTemporaryDir attachmentDirectory_;
    McpMediaStore mediaStore_;
    // Whether each provider can start a turn (#170); observed failures persist next to the journal.
    ProviderReadiness readiness_;
    AgentBroker* agents_{nullptr};
    QString agentSocket_;
    QString agentExecutable_;
    // Previews decode on their own small pool and queue: one decode may hold a 64-megapixel
    // image, and a phone asks for a screen of previews at once (#68).
    static constexpr int mediaReadThreads = 2;
    static constexpr int maximumMediaReads = 16;
    void startMediaRead(std::function<QJsonObject()> work, CommandCallback done);
    int mediaReadsInFlight_{0};
    // Last member: its destructor waits for running decodes before anything else goes away.
    QThreadPool mediaReads_;
};

} // namespace ketplus::daemon
