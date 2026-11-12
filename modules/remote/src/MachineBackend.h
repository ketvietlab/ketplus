#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

namespace ketplus::remote {

// ADR 0118: what Desktop needs from one machine, whatever carries it. This Mac's daemon socket,
// the KetPlus Relay (Control v2) and SSH to a Linux user's own daemon socket each implement it,
// so the sidebar, Agent Workspace, worktree picker and approvals bind to the selected machine and
// never to a transport.
//
// Requests and results are the daemon's local protocol objects (docs/daemon-architecture.md), the
// same shapes on every route. A backend reports a failure through the callback's error string;
// it never invents a result.
class MachineBackend : public QObject {
    Q_OBJECT

  public:
    enum class Route { Local, Relay, Ssh };
    Q_ENUM(Route)

    // ADR 0118 "Connection states". A request made before Connected is the route's to hold
    // or fail: the local route queues it in DaemonClient until the hello, as it always has.
    enum class ConnectionState { Connecting, Connected, Reconnecting, Offline };
    Q_ENUM(ConnectionState)

    using Callback = std::function<void(const QJsonObject& result, const QString& error)>;

    using QObject::QObject;
    ~MachineBackend() override = default;

    // A stable id: "local" for This Mac, the paired or SSH host's machine id otherwise.
    [[nodiscard]] virtual QString machineId() const = 0;
    [[nodiscard]] virtual Route route() const = 0;
    [[nodiscard]] virtual ConnectionState connectionState() const = 0;
    // Why the machine is not Connected, for the UI to show; empty while Connected.
    [[nodiscard]] virtual QString connectionReason() const = 0;

    // Repositories and worktrees, for the sidebar and the worktree picker.
    virtual void listRepositories(Callback callback) = 0;
    virtual void listWorktrees(const QString& sessionId, qint64 generation, int limit,
                               Callback callback) = 0;

    // The conversation list. `options` is the session.list payload (paging, live-only).
    virtual void listConversations(const QJsonObject& options, Callback callback) = 0;

    // Transcript paging: the newest window, then older pages before a cursor.
    virtual void transcriptWindow(const QString& sessionId, qint64 generation,
                                  const QJsonObject& request, Callback callback) = 0;
    virtual void transcriptPage(const QString& sessionId, qint64 generation,
                                const QJsonObject& request, Callback callback) = 0;

    // Turns. `request` is the session.start_turn / session.steer payload.
    virtual void startTurn(const QString& sessionId, qint64 generation, const QJsonObject& request,
                           Callback callback) = 0;
    virtual void steerTurn(const QString& sessionId, qint64 generation, const QJsonObject& request,
                           Callback callback) = 0;
    virtual void stopTurn(const QString& sessionId, qint64 generation, Callback callback) = 0;

    // Approvals and questions the agent is waiting on.
    // One page of pending approvals and questions after `afterId` (empty for the first).
    virtual void pendingApprovals(const QString& sessionId, qint64 generation,
                                  const QString& afterId, Callback callback) = 0;
    virtual void resolveApproval(const QString& sessionId, qint64 generation,
                                 const QString& approvalId, const QJsonObject& resolution,
                                 Callback callback) = 0;
    virtual void answerQuestion(const QString& sessionId, qint64 generation,
                                const QString& questionId, const QJsonObject& resolution,
                                Callback callback) = 0;

  signals:
    void connectionStateChanged(ketplus::remote::MachineBackend::ConnectionState state,
                                const QString& reason);
    // A session event, already decrypted and authenticated by the route.
    void eventReceived(const QJsonObject& event);
};

} // namespace ketplus::remote
