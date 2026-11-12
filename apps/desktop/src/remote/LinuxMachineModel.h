#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace ketplus {

// What Connections shows for one Linux machine (#419 scope item 4). The states are ADR 0119's;
// the SSH and Relay backends feed them, tests feed a fake. Nothing here reaches a host.
struct LinuxMachine final {
    enum class Route { Ssh, Relay };
    enum class State { Connecting, Connected, Reconnecting, Offline };
    // A failure the user can act on; Other carries the backend's own message.
    enum class Error {
        None,
        HostKeyMismatch,
        AuthenticationFailed,
        DaemonNotInstalled,
        // The daemon named another host identity than the one trusted (ADR 0119 rotation).
        IdentityChanged,
        Other
    };

    // The ADR 0119 machine id: the daemon's host key fingerprint, the same over SSH and the
    // Relay, so a host on both routes is one machine. A Relay host paired before host pins has
    // no fingerprint yet and is "relay:<relay id>" until it has one.
    QString id;
    QString name;
    QString detail;
    Route route{Route::Relay};
    State state{State::Offline};
    // The host key (SSH) or pinned host identity (Relay) as SHA256:…, once verified.
    QString fingerprint;
    // An SSH host key the host offered that did not verify (a mismatch): never shown as verified.
    QString presentedFingerprint;
    // After an identity change (Error::IdentityChanged): the daemon identity the host now names,
    // shown beside the trusted one (`id`) before the user trusts it; empty when it named none.
    QString presentedIdentity;
    Error error{Error::None};
    QString errorText;
    // The Relay's machine id while the Relay lists this host: a route attribute, never the
    // identity.
    QString relayId;
    // The SSH destination ("user@alias:port") while SSH reaches this host.
    QString sshTarget;
    // The name ssh files this machine's host key under (sshKnownHostsName of its target), for
    // the `ssh-keygen -R` hint.
    QString knownHostsName;
    // While the Relay stands in for SSH (ADR 0119 fallback): why SSH could not reach the host.
    // The row says it and offers Use SSH.
    QString sshFailure;
    // A key mismatch whose old key ketPlus accepted itself: Details offers to forget it.
    bool canForgetHostKey{false};
    // Relay: when the Linux host last reached the Relay. Connected means the Relay answers;
    // the host itself may be away (hostAway()).
    QDateTime hostLastSeen;
    // Relay: commands waiting in the outbox for the host; their outcome is still pending.
    int pendingCommands{0};
    // False when the route cannot be used (the Relay without ketPlus Pro): no Connect offered.
    bool connectable{true};

    bool operator==(const LinuxMachine&) const = default;
};

// One host the Relay's worktree index lists, with the fingerprint this client pinned for it.
struct LinuxRelayListing final {
    QString relayId;
    QString fingerprint;
    // The host's display name in the listing; the relay id when it names none.
    QString name;
    // The newest time the host refreshed its listing (its heartbeat); invalid when unknown.
    QDateTime lastSeen;
};

class LinuxMachineModel : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;

    // Every machine once: an SSH machine first, and a host the Relay lists too (same ADR 0119
    // machine id) is that SSH machine with the Relay's id as its relayId.
    [[nodiscard]] const QList<LinuxMachine>& machines() const { return machines_; }
    [[nodiscard]] const LinuxMachine* machine(const QString& id) const;
    // Replaces both routes' machines (each goes to its route's list).
    void setMachines(QList<LinuxMachine> machines);
    // Adds the machine or replaces the one with its id.
    void upsert(const LinuxMachine& machine);
    // The machines the SSH backend holds; the Relay's are kept.
    void setSshMachines(QList<LinuxMachine> machines);
    // The hosts the Relay lists, from the worktree index. SSH machines are kept; a listed host
    // that is already an SSH machine (same fingerprint) stays that one machine.
    void setRelayMachines(const QList<LinuxRelayListing>& listed);
    // The Relay backend's live state for the host it lists as `relayId`.
    void setRelayState(const QString& relayId, LinuxMachine::State state,
                       LinuxMachine::Error error = LinuxMachine::Error::None,
                       const QString& errorText = {}, int pendingCommands = 0,
                       bool connectable = true);
    // The ADR 0119 machine id for a Relay listing.
    [[nodiscard]] static QString relayMachineId(const LinuxRelayListing& listing);

    // LinuxMachinesSection attaches the backends that carry out the intents below. Until then
    // nothing receives them, so the UI offers no SSH mode and no Connect or Disconnect, and a
    // Relay machine reads "Available" as before this model.
    [[nodiscard]] bool backendsAttached() const { return backendsAttached_; }
    void setBackendsAttached(bool attached);

    // The Relay is a ketPlus Pro feature; the account service decides and this only mirrors it.
    // LinuxMachinesSection feeds it from the signed-in session's entitlements
    // (AccountEntitlements.h). Not entitled until then.
    [[nodiscard]] bool relayEntitled() const { return relayEntitled_; }
    void setRelayEntitled(bool entitled);

    // The SSH connect flow, driven by the backend while the dialog is open.
    // A host whose key is not in known_hosts: the dialog asks the user to accept it.
    void presentHostKey(const QString& host, const QString& fingerprint);
    // The connection attempt for `host` failed; the dialog shows why.
    void reportSshFailure(const QString& host, LinuxMachine::Error error, const QString& text = {});
    // The host is trusted and connected; the dialog closes.
    void reportSshConnected(const QString& host);

    // Intents, for the backends to carry out.
    void requestConnect(const QString& id) { emit connectRequested(id); }
    void requestDisconnect(const QString& id) { emit disconnectRequested(id); }
    void requestSshConnect(const QString& host) { emit sshConnectRequested(host); }
    void decideHostKey(const QString& host, const QString& fingerprint, bool accepted) {
        emit hostKeyDecided(host, fingerprint, accepted);
    }
    void cancelSshConnect(const QString& host) { emit sshConnectCancelled(host); }
    void requestRemove(const QString& id) { emit removeRequested(id); }
    // After a host key mismatch: forget the old key ketPlus accepted, then ask for the new one.
    void requestForgetHostKey(const QString& id) { emit forgetHostKeyRequested(id); }
    // After an identity change: trust the identity the daemon now names (ADR 0119 rotation).
    void requestTrustIdentity(const QString& id) { emit trustIdentityRequested(id); }
    // On a row the Relay stands in for: try SSH again (the row returns to SSH while it connects).
    void requestSshRoute(const QString& id) { emit sshRouteRequested(id); }
    // The backend asks the UI to run the SSH connect flow for `host` again (after a forgotten
    // key), so the new key is shown for acceptance.
    void promptSshConnect(const QString& host) { emit sshConnectPrompted(host); }

    // What the UI says for a state, a route and an error.
    [[nodiscard]] static QString stateText(LinuxMachine::State state);
    [[nodiscard]] static QString routeText(LinuxMachine::Route route);
    // `knownHostsName` is the name the host's key is filed under, for the ssh-keygen -R hint;
    // `host` when empty.
    [[nodiscard]] static QString errorMessage(LinuxMachine::Error error, const QString& host,
                                              const QString& text = {},
                                              const QString& knownHostsName = {});
    // The known_hosts name of a destination as the connect card takes it ("[user@]host[:port]",
    // "[v6]:port" or a bare v6 address): the host, or "[host]:port" for a port other than 22.
    [[nodiscard]] static QString knownHostsName(const QString& destination);
    // A known_hosts name as one shell word, quoted when it is bracketed.
    [[nodiscard]] static QString knownHostsCommandWord(const QString& knownHostsName);
    // The message for the machine's error, with the path ketPlus offers to fix it.
    [[nodiscard]] static QString errorMessage(const LinuxMachine& machine);
    // A Relay host that has not refreshed its listing for this long is shown as away. The host
    // heartbeats every 15 s, so two minutes is several missed beats.
    static constexpr qint64 hostAwayAfterSeconds = 120;
    // True for a Relay machine whose host has not been seen within hostAwayAfterSeconds.
    [[nodiscard]] static bool hostAway(const LinuxMachine& machine, const QDateTime& now);
    // "Host online" or "Host offline, last seen 5 min ago" for a Relay machine; empty when its
    // presence is unknown or it is reached over SSH (Connected over SSH means the host answers).
    [[nodiscard]] static QString presenceText(const LinuxMachine& machine, const QDateTime& now);
    // The activity bar's machine picker: the name with the same route and state as the row.
    [[nodiscard]] static QString pickerLabel(const LinuxMachine& machine);

  signals:
    void machinesChanged();
    void relayEntitledChanged(bool entitled);
    void backendsAttachedChanged(bool attached);
    void hostKeyPresented(const QString& host, const QString& fingerprint);
    void sshFailed(const QString& host, ketplus::LinuxMachine::Error error, const QString& text);
    void sshConnected(const QString& host);

    void connectRequested(const QString& id);
    void disconnectRequested(const QString& id);
    void sshConnectRequested(const QString& host);
    void hostKeyDecided(const QString& host, const QString& fingerprint, bool accepted);
    void sshConnectCancelled(const QString& host);
    void removeRequested(const QString& id);
    void forgetHostKeyRequested(const QString& id);
    void trustIdentityRequested(const QString& id);
    void sshRouteRequested(const QString& id);
    void sshConnectPrompted(const QString& host);

  private:
    void merge();

    QList<LinuxMachine> ssh_;
    QList<LinuxMachine> relay_;
    QList<LinuxMachine> machines_;
    bool relayEntitled_{false};
    bool backendsAttached_{false};
};

// `host` is a ~/.ssh/config alias or [user@]host[:port]; an empty result means it is valid.
[[nodiscard]] QString validateSshHost(const QString& host);

} // namespace ketplus
