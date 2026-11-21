#pragma once

#include "agents/ReviewIntegrity.h"
#include "storage/AgentMessageRecord.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

namespace ketplus::daemon {

class LocalJournal;

// What the broker reads about a conversation and asks of the session runtime (ADR 0027).
struct AgentConversation final {
    QString sessionId;
    qint64 generation{0};
    QString number;
    QString title;
    QString provider;
    QString model;
    QString worktree;
    // "ask", "full" or "read-only": a Codex reviewer needs "full" (ketplus#295).
    QString executionPolicy;
    QString state;
    QString lastActivity;
    // No open turn, background task or pending provider request: a message may start a turn.
    bool idle{false};
    // A running turn that accepts steering.
    bool steerable{false};
    // T-39: for a busy conversation that cannot be steered now, what it waits on, in words
    // ("a background task", "an approval from the user"); empty otherwise.
    QString waitingOn;
};

class AgentHostPort {
  public:
    virtual ~AgentHostPort() = default;
    [[nodiscard]] virtual std::optional<AgentConversation>
    agentConversation(const QString& sessionId) const = 0;
    [[nodiscard]] virtual QList<AgentConversation> agentConversations() const = 0;
    // Starts a turn whose provider input is `providerText`; the journal records `origin` with
    // the user.message so clients show the agent message instead of user text. Returns an error.
    virtual QString startAgentTurn(const QString& sessionId, qint64 generation,
                                   const QString& providerText, const QJsonObject& origin) = 0;
    // Steers the running turn. Returns an error when the text was not handed to the provider.
    virtual QString steerAgentTurn(const QString& sessionId, qint64 generation,
                                   const QString& providerText, const QJsonObject& origin) = 0;
    [[nodiscard]] virtual qint64 agentTurnSequence(const QString& sessionId) const = 0;
    virtual void appendAgentNotice(const QString& sessionId, qint64 generation,
                                   const QJsonObject& payload) = 0;
    // ADR 0106: merges a work_report call into the conversation's ledger and journals it.
    // Returns the tool's answer, ok or failure.
    virtual QJsonObject recordWorkReport(const QString& sessionId, qint64 generation,
                                         const QJsonObject& payload) = 0;
    // ADR 0114: the caller's team, role and lane for agents_whoami and agents_list; empty for
    // a conversation on no team.
    [[nodiscard]] virtual QJsonObject teamMembership(const QString& sessionId) const {
        (void)sessionId;
        return {};
    }
    // The number of the caller's team's `role` ("leader" or "reviewer"), empty off a team.
    [[nodiscard]] virtual QString teamRoleNumber(const QString& sessionId,
                                                 const QString& role) const {
        (void)sessionId;
        (void)role;
        return {};
    }
    // A team_* tool call (`operation` without the "agents." prefix) by the conversation.
    virtual QJsonObject teamOperation(const QString& sessionId, const QString& operation,
                                      const QJsonObject& payload) {
        (void)sessionId;
        (void)operation;
        (void)payload;
        return {
            {QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), QStringLiteral("not_on_team")},
            {QStringLiteral("error_message"), QStringLiteral("This conversation is on no team.")}};
    }
    // ADR 0114: a member of a team. Its own reviewer (ADR 0067) does not run meanwhile.
    // ADR 0114: a request the team sent (no sending conversation) was answered with `text`, or
    // its turn ended without a reply (`failure`, the reason).
    virtual void teamRequestFinished(const QString& sessionId, const AgentMessageRecord& request,
                                     const QString& text, const QString& failure) {
        (void)sessionId;
        (void)request;
        (void)text;
        (void)failure;
    }
    // ketplus#295: what the team's review request to `reviewerSession` reviews (the task's
    // worktree, branch, head_commit and owner); nothing when it reviews no task now.
    [[nodiscard]] virtual std::optional<review_integrity::Scope>
    teamReviewScope(const QString& reviewerSession, const QString& requestId) const {
        (void)reviewerSession;
        (void)requestId;
        return std::nullopt;
    }
    // ketplus#295: the reviewer changed the branch during its review; the verdict does not count.
    virtual void teamReviewTampered(const QString& reviewerSession,
                                    const AgentMessageRecord& request, const QJsonArray& changes) {
        (void)reviewerSession;
        (void)request;
        (void)changes;
    }
    // A message a conversation sent failed, expired or was never answered (`reason`).
    virtual void agentMessageFailed(const AgentMessageRecord& message, const QString& reason) {
        (void)message;
        (void)reason;
    }
    [[nodiscard]] virtual bool onTeam(const QString& sessionId) const {
        (void)sessionId;
        return false;
    }
    // ADR 0114 §2: whether the receiver's team limits the turns this message starts, in place
    // of the loop guard of ADR 0027: it comes from the team itself or from a member of the
    // same team.
    [[nodiscard]] virtual bool teamLimitsMessage(const AgentMessageRecord& message) const {
        (void)message;
        return false;
    }
    // Counts one automatic turn of a team member that its teammates' messages start against
    // the team's budget. Empty when the turn may start; otherwise why the messages wait:
    // "team_paused", or "team_budget" when the budget is used up.
    virtual QString takeTeamTurn(const QString& sessionId) {
        (void)sessionId;
        return {};
    }
    // #125: the turn's answer asks the user to approve or decide, so it ends as
    // "Waiting for you". Returns the tool's answer.
    virtual QJsonObject recordUserDecision(const QString& sessionId, qint64 generation,
                                           const QJsonObject& payload) = 0;
};

// Brokers messages between conversations on this host: capability tokens for the injected
// ketplus_agents MCP server, the agents.* operations it calls, delivery and the loop guard.
class AgentBroker final : public QObject {
    Q_OBJECT

  public:
    static constexpr qsizetype maximumBodyBytes = 32 * 1024;
    static constexpr int maximumAutomaticHop = 6;
    static constexpr int maximumChainTurns = 30;
    static constexpr int maximumHourlyTurns = 20;
    // A sender waits for the receiver to take its messages.
    static constexpr int maximumUndeliveredPerSender = 20;
    // One turn's user.message carries each message twice (prompt block and agent_messages
    // entry). A turn takes at most this many serialized bytes of them, leaving room for the
    // rest of the event inside the journal's 256 KiB limit.
    static constexpr qsizetype maximumDeliveryBytes = 240 * 1024;
    static constexpr int requestLifetimeHours = 24;
    static constexpr int expiryCheckMs = 5 * 60 * 1000;

    AgentBroker(LocalJournal* journal, AgentHostPort* host, QObject* parent = nullptr);

    // A random capability token per conversation generation; it identifies the sender.
    [[nodiscard]] QString tokenFor(const QString& sessionId, qint64 generation);
    [[nodiscard]] std::optional<std::pair<QString, qint64>>
    sessionForToken(const QString& token) const;
    // agents.whoami, agents.list, agents.send, agents.inbox and agents.work_report for the
    // token's conversation.
    [[nodiscard]] QJsonObject handle(const QString& token, const QString& operation,
                                     const QJsonObject& payload);
    [[nodiscard]] static bool isOperation(const QString& type);

    // The session runtime reports every journaled event of a conversation.
    void noteEvent(const QString& sessionId, qint64 generation, const QString& type,
                   const QJsonObject& payload);
    // A user (or client) turn was journaled: queued and held messages join it as host blocks,
    // and the turn begins a new chain. Returns the provider text to prepend, possibly empty.
    [[nodiscard]] QString takeForUserTurn(const QString& sessionId, qint64 generation);
    // `#NNNN` references in user text that name other live conversations, as provider text.
    [[nodiscard]] QString referenceBlock(const QString& sessionId, const QString& text) const;
    // Revokes the conversation's token and forgets its turn state after deletion.
    void forget(const QString& sessionId);
    // Delivers messages that were queued while the daemon was not running.
    void resume();
    // Called only by authenticated local UI IPC, never by the agents.* dispatcher.
    [[nodiscard]] QJsonObject heldMessages(const QString& sessionId, qint64 generation,
                                           const QStringList& ids) const;
    [[nodiscard]] QJsonObject releaseHeldMessages(const QString& sessionId, qint64 generation,
                                                  const QStringList& ids,
                                                  const QString& authorizationId);
    // Conversations whose running turn a message started, by session id: the first sender's
    // `number`, `title` and `kind`. Clients show the sender on the conversation's row.
    [[nodiscard]] QHash<QString, QJsonObject> automaticTurnSenders() const;
    // ADR 0114: a message of the team (assignment, digest, review, decision) to a member. It
    // starts its own chain at hop 1, so the loop guard still applies. Returns an error.
    // ADR 0114 §2: the team continued. Messages to these members that its pause, its budget
    // or an earlier build's loop guard held are delivered again, in order.
    void releaseTeamHeldMessages(const QStringList& sessionIds);
    QString postTeamMessage(const QString& fromSession, const QString& fromTitle,
                            const QString& toSession, const QString& kind, const QString& body,
                            bool expectReply, QString* messageId = nullptr, const QString& id = {});
    // ADR 0067: a reviewer was assigned; the next finished turn is compared with this moment.
    void noteReviewerAssigned(const QString& sessionId);
    // ADR 0067: the reviewer's last "Verdict:" line says "no findings".
    [[nodiscard]] static bool verdictHasNoFindings(const QString& text);
    // ADR 0067, ketplus#295: Claude, or Codex with Full access (agents/ReviewerPolicy.h).
    [[nodiscard]] static bool canReview(const AgentConversation& conversation);

  private:
    struct TurnOrigin final {
        QString chainId;
        int hop{0};
        bool automatic{false};
        // The first delivered message's sender, for an automatic turn.
        QJsonObject sender;
        QStringList expectReply;
        QString lastItemId;
        QString lastText;
        // Started by a review request: read-only, and never itself reviewed (ADR 0067).
        bool review{false};
        // The sequence of the turn the broker started; 0 for the user's own turn.
        qint64 turn{0};
        // ketplus#295: for each review request of a Codex reviewer, what it reviews and how
        // that was before the turn. Empty for any other turn.
        QHash<QString, review_integrity::Scope> scopes;
        QHash<QString, review_integrity::Snapshot> before;
    };
    // ketplus#295: the reviewed branches recorded for a Codex reviewer's next review turn.
    struct RecordedBranches final {
        QStringList messageIds;
        // The receiver's turn sequence when recorded: a turn between voids the record.
        qint64 turn{0};
        quint64 token{0};
        int pending{0};
        QHash<QString, review_integrity::Scope> scopes;
        QHash<QString, review_integrity::Snapshot> before;
    };

    [[nodiscard]] QJsonObject whoami(const AgentConversation& caller) const;
    [[nodiscard]] QJsonObject list(const AgentConversation& caller,
                                   const QJsonObject& payload) const;
    [[nodiscard]] QJsonObject send(const AgentConversation& caller, const QJsonObject& payload);
    [[nodiscard]] QJsonObject inbox(const AgentConversation& caller,
                                    const QJsonObject& payload) const;
    // Inserts a message and schedules its delivery. Returns an error message.
    QString post(AgentMessageRecord& message);
    void scheduleDelivery(const QString& sessionId);
    void processDeliveries();
    void deliverTo(const QString& sessionId);
    // Tracks the tool calls of a running turn from its item events.
    void noteToolCall(const QString& sessionId, const QString& type, const QJsonObject& payload);
    void finishTurn(const QString& sessionId, qint64 generation, const QString& status);
    // Answers the turn's expect_reply requests, except the `voided` ones.
    void answerRequests(const QString& sessionId, qint64 generation, const QString& status,
                        const TurnOrigin& origin, const QSet<QString>& voided);
    void expireOverdue();
    // Keeps the oldest messages that fit one turn; one that never fits is marked failed.
    void keepOneTurn(QList<AgentMessageRecord>& messages);
    void markState(AgentMessageRecord& message, const QString& state, const QString& reason = {});
    void notice(const AgentMessageRecord& message, const QString& state, const QString& reason);
    [[nodiscard]] static QJsonObject messageJson(const AgentMessageRecord& message);
    [[nodiscard]] static QJsonObject conversationJson(const AgentConversation& conversation);
    // ADR 0067, in AgentBrokerReviews.cpp.
    // `done` receives the tree of the worktree's files, or nothing when git failed.
    void snapshotWorktree(const QString& worktree,
                          std::function<void(std::optional<QString>)> done);
    void reviewFinishedTurn(const QString& sessionId, qint64 generation, const QString& status,
                            const TurnOrigin& origin);
    void postReview(const QString& sessionId, qint64 generation, const TurnOrigin& origin,
                    const QString& base, const QString& diff, const QStringList& files);
    // Records a review reply that should not start a turn. False when it is delivered as usual.
    bool recordReviewReply(AgentMessageRecord& reply, const AgentMessageRecord& request);
    // ketplus#295, in AgentBrokerIntegrity.cpp. True when the turn may start: no Codex review
    // in it, or its reviewed branches are recorded. Otherwise starts recording them and
    // delivers again when done.
    [[nodiscard]] bool branchesRecorded(const QString& sessionId, const AgentConversation& receiver,
                                        const QList<AgentMessageRecord>& deliverable);
    // Moves the record into the turn's origin; returns the provider text to start it with.
    [[nodiscard]] QString takeRecordedBranches(const QString& sessionId, TurnOrigin& origin,
                                               const QString& providerText);
    [[nodiscard]] std::optional<review_integrity::Scope>
    reviewScope(const AgentConversation& receiver, const AgentMessageRecord& request) const;
    // Records the branches again after the turn and answers its requests; a request whose
    // branch changed is voided.
    void checkReviewedBranches(const QString& sessionId, qint64 generation, const QString& status,
                               const TurnOrigin& origin);
    void reviewTampered(const QString& sessionId, AgentMessageRecord& request,
                        const review_integrity::Scope& scope, const QJsonArray& changes);

    LocalJournal* journal_{nullptr};
    AgentHostPort* host_{nullptr};
    QHash<QString, std::pair<QString, qint64>> sessionByToken_;
    QHash<QString, QString> tokenBySession_;
    QHash<QString, TurnOrigin> turns_;
    QSet<QString> pendingDeliveries_;
    // T-39: receivers with an urgent message that could not steer yet; each of their events
    // tries again, so it steers at the first point the provider allows.
    QSet<QString> urgentWaiting_;
    // T-39: the tool calls each conversation's running turn is in (item id → what it is), so
    // a receipt can say that a steer is read only when the call ends.
    QHash<QString, QHash<QString, QString>> toolCalls_;
    // The tree each conversation's next finished turn is compared with, and the latest
    // snapshot started for it, so an older one finishing later cannot replace it.
    QHash<QString, QString> reviewSnapshots_;
    QHash<QString, qint64> snapshotEpochs_;
    // The latest request a user typed in each conversation, for its review.
    QHash<QString, QString> userRequests_;
    QHash<QString, RecordedBranches> recordedBranches_;
    quint64 recordToken_{0};
    QTimer deliveryTimer_;
    QTimer expiryTimer_;
};

} // namespace ketplus::daemon
