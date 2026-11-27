#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus::daemon {
class CredentialStore;
}

// What runner management needs from the daemon's Control v2 client: its machine credential for
// the runner routes only, its machine keys and the capabilities the Relay answered.
namespace ketplus::daemon::runner {

struct MachineIdentity {
    QString machineId;
    QString workspaceId;
    int keyVersion = 0;
    QByteArray signingPublicKey;   // 32 raw bytes
    QByteArray agreementPublicKey; // 32 raw bytes
};

enum class RelayFailure {
    None,
    Network,        // no answer: connection, timeout or an unreadable body
    Credential,     // no machine credential (not enrolled, or it could not be renewed)
    AccountChanged, // signed out or enrolled again while the request was out
    RouteRefused,   // not a runner route; nothing was sent
};

struct RelayReply {
    int status = 0;
    QJsonValue body;
    RelayFailure failure = RelayFailure::None;
};

// The desktop runner routes of Control v2. Any other method, path or query is refused.
[[nodiscard]] bool isRunnerRoute(const QByteArray& method, const QString& path);

class RunnerRelayPort {
  public:
    using ReplyCallback = std::function<void(const RelayReply&)>;

    virtual ~RunnerRelayPort() = default;

    // The Relay listed runner.relay/1 in its last heartbeat answer.
    [[nodiscard]] virtual bool runnerRelayAvailable() const = 0;
    // Unset when this machine is not enrolled or its keys cannot be read.
    [[nodiscard]] virtual std::optional<MachineIdentity> machineIdentity() = 0;
    // Changes whenever the account or the enrollment does.
    [[nodiscard]] virtual QString accountScope() const = 0;
    // An Ed25519 signature by the machine key; empty when the key cannot be read.
    [[nodiscard]] virtual QByteArray signAsMachine(const QByteArray& bytes) = 0;
    [[nodiscard]] virtual CredentialStore& credentials() = 0;
    // `body` is sent when it is an object. Refuses a path isRunnerRoute does not accept.
    virtual void runnerRequest(const QByteArray& method, const QString& path,
                               const QJsonValue& body, ReplyCallback callback) = 0;
};

} // namespace ketplus::daemon::runner
