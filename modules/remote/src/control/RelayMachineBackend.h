#pragma once
#include "MachineBackend.h"
#include "control/CommandOutbox.h"
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <functional>

class QTimer;

namespace ketplus::remote {
class ControlV2Client;

// ADR 0119 "Relay": one paired Linux machine reached through the KetPlus Relay (Control v2).
// Reads come from ControlV2Client's host-verified projection and transcript windows. Writes are
// encrypted Control v2 commands kept in a durable CommandOutbox: each envelope is persisted
// before it is sent, only that ciphertext is ever retried, and a host-signed result is verified
// before a callback or commandCompleted() sees it, at most once per command.
class RelayMachineBackend final : public MachineBackend {
    Q_OBJECT

  public:
    struct Options {
        // The outbox file; one per machine, owner-only.
        QString outboxFile;
        int eventPollMilliseconds{1000};
        int commandPollMilliseconds{250};
        int initialBackoffMilliseconds{1000};
        int maximumBackoffMilliseconds{30000};
        // Index worktrees without a conversation verified per repository listing.
        int verifyWorktrees{32};
        // A read (pendingApprovals) is not stored; it gives up after this long.
        int readTimeoutMilliseconds{30000};
        // A result whose completion could not be recorded is read back after this long, doubling
        // to 30 s while the outbox stays unwritable.
        int recordRetryMilliseconds{1000};
        // The relay keeps a command for 15 minutes; an older outbox entry is given up.
        qint64 commandLifetimeMilliseconds{15 * 60 * 1000};
    };

    RelayMachineBackend(ControlV2Client* client, QString relayMachineId, Options options,
                        QObject* parent = nullptr);
    ~RelayMachineBackend() override;

    // The pinned host key fingerprint (ADR 0119 "Machine identity"); the relay id otherwise.
    [[nodiscard]] QString machineId() const override;
    [[nodiscard]] QString relayMachineId() const { return relayMachineId_; }
    [[nodiscard]] Route route() const override { return Route::Relay; }
    [[nodiscard]] ConnectionState connectionState() const override { return state_; }
    [[nodiscard]] QString connectionReason() const override { return reason_; }
    // Commands still waiting in the outbox.
    [[nodiscard]] QStringList pendingCommands() const;

    // Starts (or restarts after Offline) the relay link. Pending outbox commands are resumed
    // once it is Connected. Requests made while Connecting or Reconnecting wait for Connected,
    // up to commandLifetimeMilliseconds; while Offline they fail at once with the reason.
    void connectToRelay();
    void disconnectFromRelay();

    void listRepositories(Callback callback) override;
    void listWorktrees(const QString& sessionId, qint64 generation, const QJsonObject& request,
                       Callback callback) override;
    void listConversations(const QJsonObject& options, Callback callback) override;
    void transcriptWindow(const QString& sessionId, qint64 generation, const QJsonObject& request,
                          Callback callback) override;
    void transcriptPage(const QString& sessionId, qint64 generation, const QJsonObject& request,
                        Callback callback) override;
    void transcriptLargeEvent(const QString& sessionId, qint64 generation,
                              const QJsonObject& request, Callback callback) override;
    void createSession(const QJsonObject& request, Callback callback) override;
    void prepareSession(const QJsonObject& request, Callback callback) override;
    void cancelPreparedSession(const QString& sessionId, qint64 generation,
                               Callback callback) override;
    // Stops following the conversation's events until it is opened again.
    void conversationClosed(const QString& sessionId) override;
    void attachmentCommand(AttachmentOperation operation, const QString& sessionId,
                           qint64 generation, const QJsonObject& request,
                           Callback callback) override;
    void startTurn(const QString& sessionId, qint64 generation, const QJsonObject& request,
                   Callback callback) override;
    void steerTurn(const QString& sessionId, qint64 generation, const QJsonObject& request,
                   Callback callback) override;
    void stopTurn(const QString& sessionId, qint64 generation, Callback callback) override;
    void pendingApprovals(const QString& sessionId, qint64 generation, const QString& afterId,
                          Callback callback) override;
    void resolveApproval(const QString& sessionId, qint64 generation, const QString& approvalId,
                         const QJsonObject& resolution, Callback callback) override;
    void answerQuestion(const QString& sessionId, qint64 generation, const QString& questionId,
                        const QJsonObject& resolution, Callback callback) override;
    // Of the conversation operations, Control v2 carries session.rename and session.fork (the
    // host's remote operations). Archive (not a host remote operation) and attachment staging
    // through MachineBackend are not offered on this route yet; each fails with
    // unsupportedReason().
    [[nodiscard]] bool supports(Feature feature) const override;
    [[nodiscard]] QString unsupportedReason(Feature feature) const override;
    void renameConversation(const QString& sessionId, qint64 generation, const QString& title,
                            Callback callback) override;
    void forkConversation(const QString& sessionId, qint64 generation, const QString& forkSessionId,
                          Callback callback) override;
    void setConversationArchived(const QString& sessionId, qint64 generation, bool archived,
                                 Callback callback) override;

  private:
    using OutboxRoute = CommandOutbox::Route;
    void setState(ConnectionState state, const QString& reason);
    void probe();
    void worktreeCommand(const QString& kind, const QJsonObject& request, Callback callback);
    void scheduleProbe();
    // Classifies a relay failure: a fatal one goes Offline, anything else reconnects.
    void failed(const QString& error);
    [[nodiscard]] bool fatal(const QString& error) const;
    // Ends every request of this link: callbacks of outbox commands that may still run report
    // {"status": "queued"} (their outcome arrives through commandCompleted), the rest fail.
    void fence(const QString& reason);
    // True when the request may run now. Otherwise it fails (Offline) or is held until
    // Connected, when `retry` runs it again with the same callback.
    [[nodiscard]] bool hold(Callback& callback, std::function<void(Callback)> retry);
    void releaseWaiters();
    [[nodiscard]] Callback fenced(Callback callback);
    // A ControlV2Client callback that is dropped once this backend is destroyed.
    template <typename F> auto guarded(F f) {
        return [self = QPointer<RelayMachineBackend>(this),
                f = std::move(f)](const QJsonValue& value, const QString& error) mutable {
            if (self)
                f(value, error);
        };
    }
    void project(std::function<void(const QJsonObject&, const QString&)> done);
    [[nodiscard]] QJsonObject sessionRow(const QString& id, qint64 generation) const;
    // `durable`: a command that changes host state goes through the outbox; a read does not.
    void sessionCommand(const QString& kind, const QString& sessionId, qint64 generation,
                        const QJsonObject& plaintext, Callback callback, bool durable = true);
    void submitRead(const QJsonObject& envelope, Callback callback);
    void pollRead(const QJsonObject& envelope, qint64 deadline, Callback callback);
    void persistAndDeliver(OutboxRoute route, const QJsonObject& envelope, Callback callback);
    void resume();
    void deliver(const CommandOutbox::Entry& entry);
    void poll(const CommandOutbox::Entry& entry);
    void finish(const QString& id, const QJsonObject& command, const QString& error);
    void pollEvents();
    void discardRecoveredPreparation(const QString& id, const QJsonObject& command);

    QPointer<ControlV2Client> client_;
    QString relayMachineId_;
    Options options_;
    CommandOutbox outbox_;
    QString outboxError_;
    ConnectionState state_{ConnectionState::Connecting};
    QString reason_;
    bool everConnected_{false};
    // Bumped when the link goes Offline: callbacks of earlier requests are fenced.
    quint64 generation_{0};
    int attempts_{0};
    QTimer* retry_{nullptr};
    QTimer* events_{nullptr};
    QHash<QString, Callback> callbacks_;
    QSet<QString> delivering_;
    QSet<QString> recoveredPreparations_;
    QSet<QString> recoveringPreparations_;
    // Completions not yet recorded in the outbox, by id: how often a record was retried.
    QHash<QString, int> recordAttempts_;
    // Outbox commands added by this process and never handed to the relay: only these can be
    // withdrawn, because only these provably never reached it.
    QSet<QString> unsent_;
    struct Waiter {
        quint64 id{0};
        Callback callback;
        std::function<void(Callback)> retry;
    };
    QList<Waiter> waiters_;
    quint64 nextWaiter_{0};
    // The verified projection rows of this machine, by session id.
    QHash<QString, QJsonObject> sessions_;
    // Host-signed preparation results, private to this backend and never projected as a chat.
    QHash<QString, QJsonObject> prepared_;
    bool preparationSupported_{false};
    QJsonArray repositories_;
    // Sessions whose events are followed, with the last cursor seen.
    QHash<QString, qint64> watched_;
    bool pollingEvents_{false};
    // Bumped with each fence: an events poll of an earlier round never settles this one.
    quint64 eventsRound_{0};
};

} // namespace ketplus::remote
