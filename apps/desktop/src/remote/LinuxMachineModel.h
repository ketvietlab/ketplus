#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace ketplus {

// What Connections shows for one Linux machine (#419 scope item 4). The states are ADR 0118's;
// the backends (T-6) feed them, tests feed a fake. Nothing here reaches a host.
struct LinuxMachine final {
    enum class Route { Ssh, Relay };
    enum class State { Connecting, Connected, Reconnecting, Offline };
    // A failure the user can act on; Other carries the backend's own message.
    enum class Error { None, HostKeyMismatch, AuthenticationFailed, DaemonNotInstalled, Other };

    QString id;
    QString name;
    QString detail;
    Route route{Route::Relay};
    State state{State::Offline};
    // The host key (SSH) or pinned host identity (Relay) as SHA256:…, once verified.
    QString fingerprint;
    Error error{Error::None};
    QString errorText;

    bool operator==(const LinuxMachine&) const = default;
};

class LinuxMachineModel : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;

    [[nodiscard]] const QList<LinuxMachine>& machines() const { return machines_; }
    [[nodiscard]] const LinuxMachine* machine(const QString& id) const;
    void setMachines(QList<LinuxMachine> machines);
    // Adds the machine or replaces the one with its id.
    void upsert(const LinuxMachine& machine);
    // The Relay's trusted machine ids, from the worktree index. SSH machines are kept.
    void setRelayMachines(const QStringList& ids);

    // The Relay is a ketPlus Pro feature; the account service decides, this only mirrors it.
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

    // What the UI says for a state, a route and an error.
    [[nodiscard]] static QString stateText(LinuxMachine::State state);
    [[nodiscard]] static QString routeText(LinuxMachine::Route route);
    [[nodiscard]] static QString errorMessage(LinuxMachine::Error error, const QString& host,
                                              const QString& text = {});

  signals:
    void machinesChanged();
    void relayEntitledChanged(bool entitled);
    void hostKeyPresented(const QString& host, const QString& fingerprint);
    void sshFailed(const QString& host, ketplus::LinuxMachine::Error error, const QString& text);
    void sshConnected(const QString& host);

    void connectRequested(const QString& id);
    void disconnectRequested(const QString& id);
    void sshConnectRequested(const QString& host);
    void hostKeyDecided(const QString& host, const QString& fingerprint, bool accepted);
    void sshConnectCancelled(const QString& host);

  private:
    QList<LinuxMachine> machines_;
    bool relayEntitled_{true};
};

// `host` is a ~/.ssh/config alias or [user@]host[:port]; an empty result means it is valid.
[[nodiscard]] QString validateSshHost(const QString& host);

} // namespace ketplus
