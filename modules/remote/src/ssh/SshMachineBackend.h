#pragma once

#include "DaemonClient.h"
#include "DaemonMachineBackend.h"
#include "MachineBackend.h"
#include "ssh/SshTunnel.h"

#include <QJsonObject>
#include <QLocalSocket>

#include <memory>

// ADR 0119: a Linux machine reached over SSH. SshTunnel verifies the SSH host key and forwards the
// user's own daemon socket; a DaemonClient in its remote mode speaks the daemon's local protocol
// over that connection; DaemonMachineBackend maps the operations. The machine id is the daemon's
// host key fingerprint from daemon.hello, and SshMachineStore keeps it across restarts.
namespace ketplus::remote::ssh {

class SshMachineStore;

// What SshMachineBackend needs from a tunnel. SshTunnelLink adapts SshTunnel; tests drive a fake.
class SshLink : public QObject {
    Q_OBJECT

  public:
    using QObject::QObject;
    ~SshLink() override = default;

    virtual void open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual SshTunnel::State state() const = 0;
    // The SSH host key's "SHA256:..." fingerprint: the verified one, or the offending one for a
    // mismatch (fingerprintVerified() tells which).
    [[nodiscard]] virtual QString fingerprint() const = 0;
    [[nodiscard]] virtual bool fingerprintVerified() const = 0;
    // The daemon connection while Connected; null otherwise. It stays owned by the link.
    [[nodiscard]] virtual QLocalSocket* connection() const = 0;
    // The remote daemon's local credential for daemon.hello, held in memory only.
    [[nodiscard]] virtual QByteArray daemonCredential() const = 0;
    // The KetPlus-owned known_hosts file accepted keys are recorded in; empty records none.
    [[nodiscard]] virtual QString knownHostsFile() const { return {}; }
    // Asked once per unknown host key, as SshTunnel::setHostKeyPrompt().
    virtual void setHostKeyPrompt(SshTunnel::HostKeyPrompt prompt) { Q_UNUSED(prompt) }

  signals:
    void stateChanged(ketplus::remote::ssh::SshTunnel::State state);
};

class SshTunnelLink final : public SshLink {
    Q_OBJECT

  public:
    // The daemon token comes from SshTunnel::daemonCredential(): read over the verified route,
    // only while Connected.
    SshTunnelLink(SshTarget target, SshTunnelOptions options, QObject* parent = nullptr);

    [[nodiscard]] SshTunnel* tunnel() const { return tunnel_; }
    void open() override { tunnel_->open(); }
    void close() override { tunnel_->close(); }
    [[nodiscard]] SshTunnel::State state() const override { return tunnel_->state(); }
    [[nodiscard]] QString fingerprint() const override { return tunnel_->fingerprint(); }
    [[nodiscard]] bool fingerprintVerified() const override {
        return tunnel_->hostKeyStatus() == HostKeyStatus::Verified;
    }
    [[nodiscard]] QLocalSocket* connection() const override { return tunnel_->device(); }
    [[nodiscard]] QByteArray daemonCredential() const override {
        return tunnel_->daemonCredential();
    }
    [[nodiscard]] QString knownHostsFile() const override { return knownHostsFile_; }
    void setHostKeyPrompt(SshTunnel::HostKeyPrompt prompt) override {
        tunnel_->setHostKeyPrompt(std::move(prompt));
    }

  private:
    SshTunnel* tunnel_;
    QString knownHostsFile_;
};

class SshMachineBackend final : public MachineBackend {
    Q_OBJECT

  public:
    // Takes the link; `store` must outlive the backend and already hold `target`.
    SshMachineBackend(SshTarget target, SshMachineStore* store, std::unique_ptr<SshLink> link,
                      QObject* parent = nullptr);
    ~SshMachineBackend() override;

    // The trusted daemon host key fingerprint; empty for a new machine until its first hello.
    [[nodiscard]] QString machineId() const override { return machineId_; }
    [[nodiscard]] Route route() const override { return Route::Ssh; }
    [[nodiscard]] ConnectionState connectionState() const override { return state_; }
    [[nodiscard]] QString connectionReason() const override { return reason_; }

    // For Connections Details.
    [[nodiscard]] static QString routeLabel() { return QStringLiteral("SSH"); }
    [[nodiscard]] SshTarget target() const { return target_; }
    [[nodiscard]] QString targetKey() const;
    [[nodiscard]] SshTunnel::State tunnelState() const { return link_->state(); }
    // The verified SSH host key; empty unless the tunnel verified it.
    [[nodiscard]] QString sshHostKeyFingerprint() const {
        return link_->fingerprintVerified() ? link_->fingerprint() : QString();
    }
    // The key a host offered that did not verify (a mismatch); empty otherwise.
    [[nodiscard]] QString presentedSshHostKeyFingerprint() const {
        return link_->fingerprintVerified() ? QString() : link_->fingerprint();
    }
    // A hello named another daemon host key (or none): the machine is Offline until the user
    // trusts it with trustPresentedIdentity() or removes the machine.
    [[nodiscard]] bool identityChanged() const { return identityChanged_; }
    [[nodiscard]] QString presentedMachineId() const { return presented_; }
    // {"route", "target", "machine_id", "ssh_host_key_fingerprint" (verified only), "state",
    //  "reason", "presented_ssh_host_key_fingerprint" (only for a key that did not verify),
    //  "presented_machine_id" (only after a change)}; no secret.
    [[nodiscard]] QJsonObject connectionDetails() const;

    // Opens the route as a first attempt. A changed identity stays closed (trust it or remove the
    // machine); a daemon that named no identity is tried again, for a host that was updated.
    void connectToHost();
    // Offline with "Disconnected." until connectToHost().
    void disconnectFromHost();
    // ADR 0119 "Host key rotation": replaces the stored machine id with the presented one, moves
    // the conversations bound to the old id, and connects again.
    bool trustPresentedIdentity(QString* error = nullptr);
    // Records that a conversation was opened on this machine.
    bool bindConversation(const QString& conversationId, QString* error = nullptr);
    // Closes the route and forgets the machine, its id, its conversation bindings and the SSH
    // host keys accepted in ketPlus for it (from the KetPlus known_hosts file).
    bool removeMachine(QString* error = nullptr);
    // Asked for a first-use SSH host key. A key the user accepts is remembered with the machine,
    // so removeMachine() and forgetAcceptedHostKeys() can remove it again.
    void setHostKeyPrompt(SshTunnel::HostKeyPrompt prompt);
    // The re-trust path after a host key mismatch: removes the keys accepted in ketPlus for this
    // target from the KetPlus known_hosts file, so the next connectToHost() asks for the new key.
    // Returns false when ketPlus holds none: the stale key is in the user's own known_hosts.
    bool forgetAcceptedHostKeys(QString* error = nullptr);
    // True when the KetPlus known_hosts file holds a key accepted in ketPlus for this target.
    [[nodiscard]] bool holdsAcceptedHostKey() const;

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
    // As locally: the first turn in the remote worktree (its path) creates the conversation.
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

    [[nodiscard]] static QString reasonFor(SshTunnel::State state);

  signals:
    void machineIdChanged(const QString& machineId);

  private:
    void tunnelChanged();
    void daemonReady(bool ready);
    void identityRejected(const QString& presented);
    void closeLater();
    void update();
    void setState(ConnectionState state, const QString& reason);

    SshTarget target_;
    SshMachineStore* store_;
    std::unique_ptr<SshLink> link_;
    DaemonClient* client_;
    DaemonMachineBackend* operations_;
    QString machineId_;
    QString presented_;
    ConnectionState state_{ConnectionState::Connecting};
    QString reason_;
    bool everConnected_{false};
    bool identityChanged_{false};
    bool closedByUser_{false};
    QString storeError_;
};

} // namespace ketplus::remote::ssh
