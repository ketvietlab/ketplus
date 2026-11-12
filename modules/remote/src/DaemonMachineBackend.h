#pragma once

#include "MachineBackend.h"

#include <QPointer>

namespace ketplus::remote {

class DaemonClient;

// ADR 0119: a MachineBackend that speaks the daemon's local protocol to a connected socket.
// This Mac uses it over DaemonClient's Unix socket; the SSH route reuses it with a DaemonClient
// whose socket SSH forwards to the Linux user's own daemon socket. It only maps operations to
// request types and transport readiness to connection states: queueing, replay after a
// disconnect and reconnect backoff stay in DaemonClient, so This Mac behaves as before.
class DaemonMachineBackend final : public MachineBackend {
    Q_OBJECT

  public:
    using DaemonCall =
        std::function<void(const QString& type, const QJsonObject& payload,
                           const QString& sessionId, qint64 generation, Callback callback)>;

    // A daemon behind a DaemonClient: This Mac's socket (the defaults), or for the SSH route a
    // client whose socket SSH forwards, with the host's machine id and Route::Ssh. The client
    // stays owned by its creator; once it is destroyed the machine is Offline and requests fail.
    explicit DaemonMachineBackend(DaemonClient* client, QObject* parent = nullptr);
    DaemonMachineBackend(DaemonClient* client, QString machineId, Route route,
                         QObject* parent = nullptr);
    // Any transport that carries the same protocol. Its owner reports readiness with
    // setTransportReady() and setTransportUnavailable().
    DaemonMachineBackend(QString machineId, Route route, DaemonCall call,
                         QObject* parent = nullptr);

    [[nodiscard]] QString machineId() const override { return machineId_; }
    [[nodiscard]] Route route() const override { return route_; }
    [[nodiscard]] ConnectionState connectionState() const override { return state_; }
    [[nodiscard]] QString connectionReason() const override { return reason_; }

    // Ready after a hello moves to Connected. Losing readiness after a Connected session moves
    // to Reconnecting (the transport retries on its own), before the first one it stays
    // Connecting.
    void setTransportReady(bool ready, const QString& reason = {});
    // The transport cannot be reached or will not retry: Offline until it is ready again.
    void setTransportUnavailable(const QString& reason);

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
    [[nodiscard]] bool supports(Feature feature) const override;
    void renameConversation(const QString& sessionId, qint64 generation, const QString& title,
                            Callback callback) override;
    void forkConversation(const QString& sessionId, qint64 generation, const QString& forkSessionId,
                          Callback callback) override;
    void setConversationArchived(const QString& sessionId, qint64 generation, bool archived,
                                 Callback callback) override;

    // workspace.list grouped into the neutral repository shape, linked worktrees under their
    // repository (ADR 0119 "Repository grain").
    [[nodiscard]] static QJsonObject groupWorkspacesByRepository(const QJsonObject& workspaceList);

  private:
    void request(const QString& type, const QJsonObject& payload, const QString& sessionId,
                 qint64 generation, Callback callback);
    void setState(ConnectionState state, const QString& reason);

    QString machineId_;
    Route route_{Route::Local};
    DaemonCall call_;
    // The hello's features; without a client (tests) every feature is assumed.
    QPointer<DaemonClient> client_;
    bool knowsFeatures_{false};
    ConnectionState state_{ConnectionState::Connecting};
    QString reason_;
    bool everConnected_{false};
};

} // namespace ketplus::remote
