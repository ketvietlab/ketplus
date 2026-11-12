#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

namespace ketplus::remote {

// ADR 0119: what Desktop needs from one machine, whatever carries it. This Mac's daemon socket,
// the KetPlus Relay (Control v2) and SSH to a Linux user's own daemon socket each implement it,
// so the sidebar, Agent Workspace, worktree picker and approvals bind to the selected machine and
// never to a transport.
//
// Requests and results are the daemon's local protocol objects (docs/daemon-architecture.md), the
// same shapes on every route, except where an operation documents its own neutral shape. A
// backend reports a failure through the callback's error string; it never invents a result.
class MachineBackend : public QObject {
    Q_OBJECT

  public:
    enum class Route { Local, Relay, Ssh };
    Q_ENUM(Route)

    // ADR 0119 "Connection states". A request made before Connected is the route's to hold
    // or fail: the local route queues it in DaemonClient until the hello, as it always has.
    enum class ConnectionState { Connecting, Connected, Reconnecting, Offline };
    Q_ENUM(ConnectionState)

    using Callback = std::function<void(const QJsonObject& result, const QString& error)>;

    using QObject::QObject;
    ~MachineBackend() override = default;

    // A stable id: "local" for This Mac. A Linux host is named by its daemon's host key
    // fingerprint (ADR 0119 "Machine identity"), the same on both routes, so a host reached over
    // SSH and through the Relay is one machine and its conversations keep their binding.
    [[nodiscard]] virtual QString machineId() const = 0;
    [[nodiscard]] virtual Route route() const = 0;
    [[nodiscard]] virtual ConnectionState connectionState() const = 0;
    // Why the machine is not Connected, for the UI to show; empty while Connected.
    [[nodiscard]] virtual QString connectionReason() const = 0;

    // Repositories, for the sidebar, in a neutral shape no route has to fake:
    // {"repositories": [{"id", "label", "path" (only where the host path may be shown),
    // "available"}]}.
    virtual void listRepositories(Callback callback) = 0;
    // Worktrees for the worktree picker. `request` is the session.worktrees payload (query, offset,
    // limit, repository_path), as ExecutionWorktreeDialog sends it.
    virtual void listWorktrees(const QString& sessionId, qint64 generation,
                               const QJsonObject& request, Callback callback) = 0;

    // The conversation list. `options` is the session.list payload (paging, live-only).
    virtual void listConversations(const QJsonObject& options, Callback callback) = 0;

    // Transcript paging: the newest window, then older pages before a cursor. A page may carry
    // `large_event` references; the consumer resolves each with transcriptLargeEvent(). A route
    // that reassembles before delivery (the Relay) sends no references and fails that read.
    virtual void transcriptWindow(const QString& sessionId, qint64 generation,
                                  const QJsonObject& request, Callback callback) = 0;
    virtual void transcriptPage(const QString& sessionId, qint64 generation,
                                const QJsonObject& request, Callback callback) = 0;
    // One fragment of a large event: the session.transcript_large_event payload.
    virtual void transcriptLargeEvent(const QString& sessionId, qint64 generation,
                                      const QJsonObject& request, Callback callback) = 0;

    // A new conversation in a worktree of this machine: {"worktree_id", "prompt", "options"}
    // with the `id` listWorktrees() or listRepositories() reported (the host path on the local
    // and SSH routes), plus "repository_id" on the Relay. The result names the new session_id.
    virtual void createSession(const QJsonObject& request, Callback callback) = 0;

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
    // The repositories or worktrees the route can verify changed: list them again.
    void catalogChanged();
};

} // namespace ketplus::remote
