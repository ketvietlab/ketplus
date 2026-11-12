#pragma once
#include "MachineBackend.h"
#include "control/CommandOutbox.h"
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QSet>

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
    // once it is Connected.
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

  signals:
    // A command's verified host result, once. Commands resumed after a restart have no
    // callback, so this is how their outcome reaches the UI.
    void commandCompleted(const QString& commandId, const QJsonObject& result,
                          const QString& error);

  private:
    using OutboxRoute = CommandOutbox::Route;
    void setState(ConnectionState state, const QString& reason);
    void probe();
    void scheduleProbe();
    // Classifies a relay failure: a fatal one goes Offline, anything else reconnects.
    void failed(const QString& error);
    [[nodiscard]] bool fatal(const QString& error) const;
    [[nodiscard]] bool requireConnected(const Callback& callback) const;
    [[nodiscard]] Callback fenced(Callback callback);
    void project(std::function<void(const QJsonObject&, const QString&)> done);
    [[nodiscard]] QJsonObject sessionRow(const QString& id, qint64 generation) const;
    void sessionCommand(const QString& kind, const QString& sessionId, qint64 generation,
                        const QJsonObject& plaintext, Callback callback);
    void persistAndDeliver(OutboxRoute route, const QJsonObject& envelope, Callback callback);
    void resume();
    void deliver(const CommandOutbox::Entry& entry);
    void poll(const CommandOutbox::Entry& entry);
    void finish(const QString& id, const QJsonObject& command, const QString& error);
    void pollEvents();

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
    // The verified projection rows of this machine, by session id.
    QHash<QString, QJsonObject> sessions_;
    QJsonArray repositories_;
    // Sessions whose events are followed, with the last cursor seen.
    QHash<QString, qint64> watched_;
    bool pollingEvents_{false};
};

} // namespace ketplus::remote
