#pragma once
#include "ssh/SshHostKeys.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QProcess;
class QTemporaryDir;
class QTimer;

// A route to a Linux host's KetPlus daemon over the system OpenSSH client (docs/ssh-transport.md).
// SshTunnel verifies the host key before any daemon byte, resolves the daemon socket the way the
// user unit does, bridges it through `ssh -W` to a private local socket and exposes that socket.
// It also reads the daemon's local token over the same verified connection.
namespace ketplus::remote::ssh {

// What KetPlus stores for a host: no secret. The key and the agent stay with OpenSSH.
struct SshTarget {
    // A Host from ~/.ssh/config or a host name.
    QString alias;
    // Empty and 0 leave the user and port to ssh_config.
    QString user;
    int port{0};
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static SshTarget fromJson(const QJsonObject& value);
};

enum class HostKeyStatus { None, Verified, Mismatch, Unknown };

struct SshTunnelOptions {
    QString sshProgram{QStringLiteral("ssh")};
    QString keygenProgram{QStringLiteral("ssh-keygen")};
    // The KetPlus-owned known_hosts file that accepted keys are recorded in; empty uses
    // defaultKnownHostsFile().
    QString knownHostsFile;
    // Options placed before the destination of every ssh call (tests pass {"-F", config}).
    QStringList sshArguments;
    int stepTimeoutMilliseconds{30000};
    int connectTimeoutSeconds{10};
    int initialBackoffMilliseconds{1000};
    int maximumBackoffMilliseconds{30000};
    // The forwarder must stay up this long (or carry daemon bytes) before the tunnel reports
    // Connected; a bridge that fails at once never flaps through Connected.
    int settleMilliseconds{500};
};

class SshTunnel : public QObject {
    Q_OBJECT

  public:
    enum class State {
        Idle,
        Connecting,
        Connected,
        Offline,
        AuthFailed,
        HostKeyMismatch,
        // The host key was unknown and the accept callback refused it (or none was set).
        HostKeyRejected,
        // No daemon socket, or a socket without a local token (not installed or initialised).
        DaemonSocketMissing,
        // The host's daemon token is readable by other users: refused, never retried.
        DaemonTokenInsecure,
    };
    Q_ENUM(State)

    using Options = SshTunnelOptions;

    // Asked once per unknown key; decide(true) records it in the KetPlus known_hosts file.
    using HostKeyPrompt = std::function<void(const HostKey& key, std::function<void(bool)> decide)>;

    [[nodiscard]] static QString defaultKnownHostsFile();

    explicit SshTunnel(SshTarget target, Options options = {}, QObject* parent = nullptr);
    ~SshTunnel() override;

    void setHostKeyPrompt(HostKeyPrompt prompt);
    // Verifies, locates and forwards; reconnects with bounded backoff after Offline or
    // DaemonSocketMissing. AuthFailed and the host-key states wait for the next open().
    void open();
    void close();

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] HostKeyStatus hostKeyStatus() const { return hostKeyStatus_; }
    // The verified host key's "SHA256:..." fingerprint, or the offending one for a mismatch.
    [[nodiscard]] QString fingerprint() const { return fingerprint_; }
    [[nodiscard]] QString remoteSocketPath() const { return remoteSocket_; }
    // A fixed, secret-free description of the last failure.
    [[nodiscard]] QString lastError() const { return lastError_; }
    // The daemon connection while Connected; null otherwise. The caller must send daemon.hello
    // on it promptly: the daemon drops a connection without a hello after 10 s.
    [[nodiscard]] QLocalSocket* device() const;
    // The host daemon's local token while Connected; empty otherwise. It is held in memory
    // only, cleared on close, teardown and failure, and never logged or persisted.
    [[nodiscard]] QByteArray daemonCredential() const;
    // Reconnects scheduled since the last Connected state.
    [[nodiscard]] int reconnectAttempts() const { return reconnects_; }

  signals:
    void stateChanged(ketplus::remote::ssh::SshTunnel::State state);

  private:
    using StepDone = std::function<void(bool finished, int exitCode, const QByteArray& out,
                                        const QByteArray& err)>;
    void run(const QString& program, const QStringList& arguments, StepDone done);
    [[nodiscard]] QStringList destinationOptions() const;
    // `stdio`: the call's stdin and stdout carry the daemon connection (`ssh -W`).
    [[nodiscard]] QStringList verifiedOptions(bool stdio = false) const;
    void resolve();
    // A second probe restricted to the known key types; `offered` is the key the first one saw.
    void probe(const QStringList& algorithms, const HostKey& offered = {});
    void lookup(const HostKey& key, bool reprobed);
    void verified(const HostKey& key, bool certificate);
    void mismatch(const HostKey& key, const QString& error);
    void locate();
    void forward();
    // Reports Connected once the bridge has carried daemon bytes or outlived the settle period.
    void settle(quint64 generation);
    void fail(State state, const QString& error);
    void lost(const QString& error);
    void teardown();
    void setState(State state);
    [[nodiscard]] static State classify(const QByteArray& err);

    SshTarget target_;
    Options options_;
    HostKeyPrompt prompt_;
    State state_{State::Idle};
    HostKeyStatus hostKeyStatus_{HostKeyStatus::None};
    HostKey hostKey_;
    // Verified through a @cert-authority line: ssh checks the certificate itself.
    bool certificate_{false};
    QByteArray credential_;
    QString fingerprint_;
    QString remoteSocket_;
    QString lastError_;
    QStringList knownFiles_;
    // Bumped by open() and close(): callbacks of an older attempt are dropped.
    quint64 generation_{0};
    int reconnects_{0};
    // The forwarder has passed daemon bytes in this attempt.
    bool carried_{false};
    std::unique_ptr<QTemporaryDir> work_;
    QPointer<QProcess> step_;
    QPointer<QProcess> forwarder_;
    QPointer<QLocalServer> bridge_;
    // The bridge's end of the local socket, pumped to and from the forwarder's stdio.
    QPointer<QLocalSocket> peer_;
    QPointer<QLocalSocket> socket_;
    QTimer* retry_{nullptr};
};

} // namespace ketplus::remote::ssh
