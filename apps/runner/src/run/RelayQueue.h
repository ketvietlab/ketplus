#pragma once

#include "delivery/DeliveryPorts.h"
#include "peer/PeerPorts.h"
#include "run/RunPorts.h"

#include <QList>
#include <QObject>

#include <optional>

// Every Relay HTTP call of the run, one at a time. A call blocks in a nested event loop, so
// timers, socket messages, peer frames and command output may ask for another meanwhile: it
// waits in the queue and starts from the outer loop once the running one has answered. Answers
// are handed out after the call returned, never from inside a send.
namespace ketplus::runner::app {

class RelayQueueListener {
  public:
    virtual ~RelayQueueListener() = default;
    // The heartbeat to send now: built when its turn comes, not when it was asked for.
    [[nodiscard]] virtual QJsonObject heartbeatBody() = 0;
    virtual void heartbeatAnswered(const AccessTokens::Call& call) = 0;
    virtual void leaseAnswered(const delivery::LeaseCall& call,
                               const delivery::CallResult& result) = 0;
    // The Relay refused the token for good, or the credential could not be stored.
    virtual void tokenFailed(AccessTokens::Outcome outcome) = 0;
    virtual void pairingsChecked(std::optional<ExitCode> exit) = 0;
};

class RelayQueue final : public delivery::RelayCalls, public peer::TokenSource {
  public:
    RelayQueue(RelayPort& port, RelayQueueListener& listener);
    RelayQueue(const RelayQueue&) = delete;
    RelayQueue& operator=(const RelayQueue&) = delete;

    // At most one heartbeat waits: asking again while one waits changes nothing.
    void heartbeat();
    // Likewise for the check of pending pins.
    void pairings();
    void send(const delivery::LeaseCall& call) override; // delivery::RelayCalls
    void current(Answer answer) override;                // peer::TokenSource
    void renew(Answer answer) override;

    // No call starts any more; those waiting are dropped.
    void close();
    // Nothing runs and nothing waits.
    [[nodiscard]] bool idle() const { return !m_running && m_items.isEmpty(); }
    [[nodiscard]] bool running() const { return m_running; }
    [[nodiscard]] bool heartbeatWaiting() const;

  private:
    struct Item {
        enum class Kind { Heartbeat, Pairings, Lease, Token };
        Kind kind = Kind::Heartbeat;
        delivery::LeaseCall call; // Lease
        bool renew = false;       // Token
        Answer answer;            // Token
    };

    void enqueue(Item item);
    void schedule();
    void pump();
    void run(const Item& item);

    RelayPort& m_port;
    RelayQueueListener& m_listener;
    QList<Item> m_items;
    bool m_running = false;
    bool m_scheduled = false;
    bool m_closed = false;
    QObject m_home; // queued pumps die with the queue
};

} // namespace ketplus::runner::app
