#pragma once

#include "runner/RunnerRelayPort.h"

namespace ketplus::daemon {
class CloudControlClient;
}

namespace ketplus::daemon::runner {

// The runner port of the daemon's Control v2 client: its machine credential, keys and the
// capabilities from its heartbeat, for the runner routes only.
class CloudRunnerRelay final : public RunnerRelayPort {
  public:
    explicit CloudRunnerRelay(CloudControlClient& client) : client_(client) {}

    [[nodiscard]] bool runnerRelayAvailable() const override;
    [[nodiscard]] std::optional<MachineIdentity> machineIdentity() override;
    [[nodiscard]] QString accountScope() const override;
    [[nodiscard]] QByteArray signAsMachine(const QByteArray& bytes) override;
    [[nodiscard]] CredentialStore& credentials() override;
    void runnerRequest(const QByteArray& method, const QString& path, const QJsonValue& body,
                       ReplyCallback callback) override;

  private:
    CloudControlClient& client_;
};

} // namespace ketplus::daemon::runner
