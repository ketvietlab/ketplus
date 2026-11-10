#pragma once

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
    QString state;
    QString lastActivity;
    // No open turn, background task or pending provider request: a message may start a turn.
    bool idle{false};
    // A running turn that accepts steering.
    bool steerable{false};
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
    // agents.whoami, agents.list, agents.send and agents.inbox for the token's conversation.
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
    // ADR 0067: a reviewer was assigned; the next finished turn is compared with this moment.
    void noteReviewerAssigned(const QString& sessionId);
    // ADR 0067: only Claude enforces the read-only review guard on every tool call. Codex keeps
    // process-wide MCP servers (the browser) that approve themselves; Cursor forwards prompts.
    [[nodiscard]] static bool enforcesReviewGuard(const QString& provider) {
        return provider == QStringLiteral("claude");
    }

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
    void finishTurn(const QString& sessionId, qint64 generation, const QString& status);
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

    LocalJournal* journal_{nullptr};
    AgentHostPort* host_{nullptr};
    QHash<QString, std::pair<QString, qint64>> sessionByToken_;
    QHash<QString, QString> tokenBySession_;
    QHash<QString, TurnOrigin> turns_;
    QSet<QString> pendingDeliveries_;
    // The tree each conversation's next finished turn is compared with, and the latest
    // snapshot started for it, so an older one finishing later cannot replace it.
    QHash<QString, QString> reviewSnapshots_;
    QHash<QString, qint64> snapshotEpochs_;
    // The latest request a user typed in each conversation, for its review.
    QHash<QString, QString> userRequests_;
    QTimer deliveryTimer_;
    QTimer expiryTimer_;
};

} // namespace ketplus::daemon
