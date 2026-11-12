#pragma once
#include "ssh/SshHostKeys.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QLocalSocket;
class QProcess;
class QTemporaryDir;
class QTimer;

// A route to a Linux host's KetPlus daemon over the system OpenSSH client (docs/ssh-transport.md).
// SshTunnel verifies the host key before any daemon byte, resolves the daemon socket the way the
// user unit does, forwards it to a private local socket and exposes that socket as a QIODevice.
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
        DaemonSocketMissing,
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
    // The daemon connection while Connected; null otherwise.
    [[nodiscard]] QIODevice* device() const;
    // Reconnects scheduled since the last Connected state.
    [[nodiscard]] int reconnectAttempts() const { return reconnects_; }

  signals:
    void stateChanged(ketplus::remote::ssh::SshTunnel::State state);

  private:
    using StepDone = std::function<void(bool finished, int exitCode, const QByteArray& out,
                                        const QByteArray& err)>;
    void run(const QString& program, const QStringList& arguments, StepDone done);
    [[nodiscard]] QStringList destinationOptions() const;
    [[nodiscard]] QStringList verifiedOptions() const;
    void resolve();
    void probe(const QStringList& algorithms);
    void lookup(const HostKey& key, bool reprobed);
    void verified(const HostKey& key);
    void locate();
    void forward();
    void attach(int remaining);
    void fail(State state, const QString& error);
    void lost(const QString& error);
    void teardown();
    void setState(State state);
    [[nodiscard]] State classify(const QByteArray& err) const;

    SshTarget target_;
    Options options_;
    HostKeyPrompt prompt_;
    State state_{State::Idle};
    HostKeyStatus hostKeyStatus_{HostKeyStatus::None};
    HostKey hostKey_;
    QString fingerprint_;
    QString remoteSocket_;
    QString lastError_;
    QStringList knownFiles_;
    // Bumped by open() and close(): callbacks of an older attempt are dropped.
    quint64 generation_{0};
    int reconnects_{0};
    std::unique_ptr<QTemporaryDir> work_;
    QPointer<QProcess> step_;
    QPointer<QProcess> forwarder_;
    QPointer<QLocalSocket> socket_;
    QTimer* retry_{nullptr};
};

} // namespace ketplus::remote::ssh
