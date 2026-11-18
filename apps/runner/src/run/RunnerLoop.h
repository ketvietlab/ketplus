#pragma once

#include "Environment.h"
#include "ExitCode.h"
#include "delivery/GuardedAcceptedJobs.h"
#include "delivery/JobDelivery.h"
#include "delivery/JobSessionLink.h"
#include "peer/PeerChannels.h"
#include "peer/PeerConnector.h"
#include "peer/RelayLink.h"
#include "run/Backoff.h"
#include "run/RelayQueue.h"
#include "run/RunClocks.h"
#include "run/RunPorts.h"
#include "session/JobSession.h"
#include "state/JobJournalFile.h"

#include <functional>
#include <memory>

// `ketplus-runner run`: the job pieces wired into one event-driven loop. Everything that reaches
// outside the process (Relay calls and socket, peer transport, command runner, disk, timers,
// stop signals) comes in through RunnerDeps, so the same loop runs over fakes in tests.
namespace ketplus::runner::app {

inline constexpr qint64 heartbeatIntervalMs = 30'000;
inline constexpr qint64 heartbeatSpacingMs = 2'000;
inline constexpr qint64 startupCleanupBoundMs = 90'000;
inline constexpr qint64 stopPollMs = 250;

// The snapshot receiver, with where its answers go and its between-jobs housekeeping.
struct ReceiverHost {
    session::SnapshotReceiver& receiver;
    std::function<void(std::function<void(const QString&, const QStringList&)> ready,
                       std::function<void(const QString&)> failed)>
        answerTo;
    std::function<void()> idle;
};

struct RunnerDeps {
    const Environment& environment; // out, err, random, runnerVersion
    RelayPort& relay;
    std::function<std::unique_ptr<peer::RelaySocket>(peer::RelaySocketEvents&)> socket;
    peer::PeerTransportFactory& transports;
    const peer::OfferSigner& signer;
    const peer::PinLookup& pins;
    const delivery::RequesterTrust& trust;
    JobJournalFile& journal;
    session::AcceptedJobs& accepted;
    QString acceptedError; // set: the accepted-jobs record could not be read
    ReceiverHost receiver;
    CommandHost& command;
    TimerHub& timers;
    session::RunnerInfo info;    // labels: every label, user and system
    QStringList heartbeatLabels; // the user's labels, as registered
    std::function<QString()> newAcquireId;
    std::function<int()> stopSignals; // SIGTERM and SIGINT received so far
};

class RunnerLoop final : public RelayQueueListener,
                         public peer::RelayLinkListener,
                         public peer::WakeupListener,
                         public peer::PeerChannelListener,
                         public peer::SignalSender,
                         public peer::RelaySocketEvents,
                         public delivery::SessionLink {
  public:
    enum class Startup { NotStarted, CleaningUp, Running };

    explicit RunnerLoop(RunnerDeps deps);
    ~RunnerLoop() override;
    RunnerLoop(const RunnerLoop&) = delete;
    RunnerLoop& operator=(const RunnerLoop&) = delete;

    // Starts the clean-up, the Relay socket and the heartbeat. `finished` is called once with
    // the exit code; nothing is destroyed from inside it.
    void start(std::function<void(ExitCode)> finished);

    // What the next heartbeat reports: "idle", "busy" or "draining".
    [[nodiscard]] QString reportedState() const;
    // Whether an acquire may start now (the delivery adds its own conditions).
    [[nodiscard]] bool mayAcquire() const;
    [[nodiscard]] Startup startup() const { return m_startup; }
    [[nodiscard]] bool shuttingDown() const { return m_shuttingDown; }
    [[nodiscard]] const delivery::JobDelivery& delivery() const { return m_delivery; }
    [[nodiscard]] const session::JobSession& session() const { return m_session; }
    [[nodiscard]] const RelayQueue& queue() const { return m_queue; }

    // RelayQueueListener.
    [[nodiscard]] QJsonObject heartbeatBody() override;
    void heartbeatAnswered(const AccessTokens::Call& call) override;
    void leaseAnswered(const delivery::LeaseCall& call,
                       const delivery::CallResult& result) override;
    void tokenFailed(AccessTokens::Outcome outcome) override;
    void pairingsChecked(std::optional<ExitCode> exit) override;

    // RelayLinkListener.
    void ready() override;
    void lost() override;
    void refused(int status) override;
    void runnerRemoved() override;
    void tokenRejected() override;
    void signal(const QJsonObject& message) override;

    // WakeupListener.
    void jobAvailable(const QString& jobId) override;
    void jobCancel(const QString& jobId, const QString& leaseId) override;
    void runnerPaired(const QString& ticketId, const QString& pairState,
                      const QString& approvalSignature) override;
    void runnerUnpaired(const QString& machineId) override;

    // PeerChannelListener.
    void channelOpened(const QString& connectionId) override;
    void channelLost(const QString& reason) override;
    void frameReceived(protocol::Channel channel, const QByteArray& message) override;
    void transferReceived(quint64 transferId, const QByteArray& bytes) override;
    void eventsWritable() override;
    void attemptFailed(const QString& reason) override;
    void gaveUp(const QString& reason) override;

    // SignalSender: the connector's signals go out on the Relay socket.
    [[nodiscard]] bool relayReady() const override;
    bool sendSignal(const QJsonObject& message) override;

    // RelaySocketEvents: forwarded to the Relay link.
    void socketOpened() override;
    void socketText(const QString& text) override;
    void socketClosed(int code) override;
    void socketRefused(int status) override;

    // SessionLink: the delivery's session, which also names the peer request.
    [[nodiscard]] bool open(const delivery::HeldLease& lease) override;
    [[nodiscard]] bool withdraw() override;
    void leaseStarted(const QString& startedAt) override;
    void leaseStartRefused(const QString& code) override;
    void leaseLost(const QString& code, const QString& reason) override;
    [[nodiscard]] QString phase() const override;

  private:
    void fired(TimerKey key);
    void cleanupDone(bool clean, bool bounded);
    void restore();
    void update();
    void releaseUnspecifiedLease();
    void askHeartbeat();
    void heartbeatSoon();
    void checkStop();
    void beginShutdown();
    void finish(ExitCode code);
    void tokenFailure(AccessTokens::Outcome outcome);
    [[nodiscard]] bool draining() const;
    [[nodiscard]] bool holding() const;
    [[nodiscard]] qint64 nowMs() const;

    RunnerDeps m_deps;
    RelayQueue m_queue;
    HubSessionClock m_sessionClock;
    HubDeliveryClock m_deliveryClock;
    HubPeerClock m_peerClock;
    delivery::NeverRelaunch m_restart;
    delivery::JobSessionLink m_sessionLink;
    delivery::JobDelivery m_delivery;
    delivery::GuardedAcceptedJobs m_accepted;
    peer::PeerChannels m_channels;
    peer::PeerConnector m_connector;
    session::JobSession m_session;
    std::unique_ptr<peer::RelaySocket> m_socket;
    std::unique_ptr<peer::RelayLink> m_link;

    std::function<void(ExitCode)> m_finished;
    Startup m_startup = Startup::NotStarted;
    bool m_cleanupClean = false;
    bool m_shuttingDown = false;
    bool m_done = false;
    bool m_wasHolding = false;
    bool m_linkStarted = false;

    Backoff m_heartbeatBackoff;
    bool m_heartbeatAsked = false;
    bool m_heartbeatFailing = false;
    std::optional<qint64> m_lastHeartbeatMs;
    QString m_sentState;
    bool m_disabled = false;
};

} // namespace ketplus::runner::app
