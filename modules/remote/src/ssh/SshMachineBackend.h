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
    // The verified SSH host key's "SHA256:..." fingerprint, or the offending one for a mismatch.
    [[nodiscard]] virtual QString fingerprint() const = 0;
    // The daemon connection while Connected; null otherwise. It stays owned by the link.
    [[nodiscard]] virtual QLocalSocket* connection() const = 0;
    // The remote daemon's local credential for daemon.hello, held in memory only.
    [[nodiscard]] virtual QByteArray daemonCredential() const = 0;

  signals:
    void stateChanged(ketplus::remote::ssh::SshTunnel::State state);
};

class SshTunnelLink final : public SshLink {
    Q_OBJECT

  public:
    // `credential` reads the remote daemon's token over the verified route.
    SshTunnelLink(SshTarget target, SshTunnelOptions options,
                  DaemonClient::CredentialSource credential, QObject* parent = nullptr);

    [[nodiscard]] SshTunnel* tunnel() const { return tunnel_; }
    void open() override { tunnel_->open(); }
    void close() override { tunnel_->close(); }
    [[nodiscard]] SshTunnel::State state() const override { return tunnel_->state(); }
    [[nodiscard]] QString fingerprint() const override { return tunnel_->fingerprint(); }
    [[nodiscard]] QLocalSocket* connection() const override;
    [[nodiscard]] QByteArray daemonCredential() const override;

  private:
    SshTunnel* tunnel_;
    DaemonClient::CredentialSource credential_;
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
    [[nodiscard]] QString sshHostKeyFingerprint() const { return link_->fingerprint(); }
    // A hello named another daemon host key (or none): the machine is Offline until the user
    // trusts it with trustPresentedIdentity() or removes the machine.
    [[nodiscard]] bool identityChanged() const { return identityChanged_; }
    [[nodiscard]] QString presentedMachineId() const { return presented_; }
    // {"route", "target", "machine_id", "ssh_host_key_fingerprint", "state", "reason",
    //  "presented_machine_id" (only after a change)}; no secret.
    [[nodiscard]] QJsonObject connectionDetails() const;

    // Opens the route; does nothing while identityChanged().
    void connectToHost();
    // Offline with "Disconnected." until connectToHost().
    void disconnectFromHost();
    // ADR 0119 "Host key rotation": replaces the stored machine id with the presented one, moves
    // the conversations bound to the old id, and connects again.
    bool trustPresentedIdentity(QString* error = nullptr);
    // Records that a conversation was opened on this machine.
    bool bindConversation(const QString& conversationId, QString* error = nullptr);
    // Closes the route and forgets the machine, its id and its conversation bindings.
    bool removeMachine(QString* error = nullptr);

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
