#pragma once

#include "delivery/DeliveryPorts.h"

#include <functional>

// Job delivery's Relay calls over the RelayClient, with an access token. Each call blocks like
// every Relay call of the runner; its answer is handed back from the event loop, never from
// inside send().
namespace ketplus::runner::app {

class AccessTokens;
class RelayClient;

class RelayLeaseCalls final : public delivery::RelayCalls {
  public:
    using Answer = std::function<void(const delivery::LeaseCall&, const delivery::CallResult&)>;
    RelayLeaseCalls(RelayClient& relay, AccessTokens& tokens, Answer answer)
        : m_relay(relay), m_tokens(tokens), m_answer(std::move(answer)) {}

    void send(const delivery::LeaseCall& call) override;
    // The call without the event loop, for tests and the queued send.
    [[nodiscard]] delivery::CallResult call(const delivery::LeaseCall& call);

  private:
    RelayClient& m_relay;
    AccessTokens& m_tokens;
    Answer m_answer;
};

} // namespace ketplus::runner::app
