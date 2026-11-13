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

    // Conversation operations a route may not carry yet. The UI offers each one everywhere and
    // disables it, with unsupportedReason(), where supports() is false (ADR 0119 "Parity").
    enum class Feature { Rename, Fork, Archive, Attachments, SessionPreparation };
    Q_ENUM(Feature)

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
    // ADR 0120: inactive first-turn attachment scope. Requires an advertised capability;
    // request includes a stable client_request_id, worktree_id and initial options.
    virtual void prepareSession(const QJsonObject& request, Callback callback);
    virtual void cancelPreparedSession(const QString& sessionId, qint64 generation,
                                       Callback callback);

    // The conversation is no longer open or selected: a route that follows its events stops.
    virtual void conversationClosed(const QString& sessionId) { Q_UNUSED(sessionId); }
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

    // Whether this route carries `feature` now (a daemon names its features in the hello, so a
    // route that is not Connected may not know yet). The default carries none.
    [[nodiscard]] virtual bool supports(Feature feature) const {
        Q_UNUSED(feature);
        return false;
    }
    // Why a feature is disabled, for its tooltip.
    [[nodiscard]] virtual QString unsupportedReason(Feature feature) const;
    // Existing attachment.stage protocol (ADR 0007). A queued response is not completion;
    // consumers must wait for commandCompleted before advancing the upload sequence.
    enum class AttachmentOperation { Begin, Chunk, Commit, Cancel };
    virtual void attachmentCommand(AttachmentOperation operation, const QString& sessionId,
                                   qint64 generation, const QJsonObject& request,
                                   Callback callback);
    [[nodiscard]] static QString attachmentCommandKind(AttachmentOperation operation);

    // session.rename {title}.
    virtual void renameConversation(const QString& sessionId, qint64 generation,
                                    const QString& title, Callback callback);
    // session.fork {fork_session_id}: a copy of the conversation under the client-chosen id, so
    // a retried request cannot fork twice. The result names the new session_id and generation.
    virtual void forkConversation(const QString& sessionId, qint64 generation,
                                  const QString& forkSessionId, Callback callback);
    // session.set_archived {archived}.
    virtual void setConversationArchived(const QString& sessionId, qint64 generation, bool archived,
                                         Callback callback);

  signals:
    void connectionStateChanged(ketplus::remote::MachineBackend::ConnectionState state,
                                const QString& reason);
    // A session event, already decrypted and authenticated by the route, in the local daemon's
    // session.event shape on every route: session_id, session_generation, cursor, turn_sequence,
    // sent_at and payload {event_type, data}. On the Relay route sent_at is the relay's
    // unsigned occurredAt (a display hint); the other fields are host-signed.
    void eventReceived(const QJsonObject& event);
    // The repositories or worktrees the route can verify changed: list them again.
    void catalogChanged();
    // A write whose callback answered {"status": "queued", "command_id"} finished on the host:
    // its verified result or error, once. Commands resumed after a restart have no callback, so
    // this is how their outcome reaches the UI. Routes that answer every write directly (local,
    // SSH) never emit it.
    void commandCompleted(const QString& commandId, const QJsonObject& result,
                          const QString& error);
};

} // namespace ketplus::remote
