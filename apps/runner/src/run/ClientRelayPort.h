#pragma once

#include "flow/PendingPins.h"
#include "relay/RelayLeaseCalls.h"
#include "run/RunPorts.h"

// The Relay port over the blocking RelayClient and the runner's access tokens.
namespace ketplus::runner::app {

class ClientRelayPort final : public RelayPort {
  public:
    explicit ClientRelayPort(PairingContext& context)
        : m_context(context), m_leases(context.relay, context.tokens, {}) {}

    [[nodiscard]] AccessTokens::Call heartbeat(const QJsonObject& body) override;
    [[nodiscard]] delivery::CallResult lease(const delivery::LeaseCall& call) override;
    [[nodiscard]] Token token(bool renew) override;
    [[nodiscard]] std::optional<ExitCode> pairings() override;

  private:
    PairingContext& m_context;
    RelayLeaseCalls m_leases;
};

} // namespace ketplus::runner::app
