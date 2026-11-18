#include "run/RunnerLoop.h"

#include "flow/PendingPins.h"
#include "protocol/Capabilities.h"

#include <QJsonArray>

#include <algorithm>

namespace ketplus::runner::app {

namespace {

const QString idleState = QStringLiteral("idle");
const QString busyState = QStringLiteral("busy");
const QString drainingState = QStringLiteral("draining");

} // namespace

RunnerLoop::RunnerLoop(RunnerDeps deps)
    : m_deps(std::move(deps)), m_queue(m_deps.relay, *this), m_sessionClock(m_deps.timers),
      m_deliveryClock(m_deps.timers), m_peerClock(m_deps.timers),
      m_delivery(m_deps.info.labels,
                 delivery::DeliveryPorts{
                     m_queue, *this, m_deps.journal, m_deps.trust, m_restart, m_deliveryClock,
                     m_deps.environment.random, m_deps.newAcquireId,
                     [this](const QString& problem) { m_deps.environment.err(problem); }}),
      m_accepted(m_deps.accepted, m_delivery), m_channels(m_peerClock, *this),
      m_connector(*this, m_peerClock, m_deps.transports, m_deps.signer, m_deps.pins, m_channels),
      m_session(m_deps.info,
                session::SessionPorts{m_channels, m_delivery.lease(), m_deps.receiver.receiver,
                                      m_deps.command.runner(), m_sessionClock, m_accepted}),
      m_heartbeatBackoff(m_deps.environment.random) {
    m_sessionLink.attach(m_session);
    m_channels.attach(m_connector);
    m_socket = m_deps.socket(*this);
    m_link = std::make_unique<peer::RelayLink>(*m_socket, m_queue, m_peerClock,
                                               Backoff(m_deps.environment.random), *this, *this);
    m_deps.receiver.answerTo(
        [this](const QString& workspace, const QStringList& warnings) {
            m_session.snapshotReady(workspace, warnings);
            update();
        },
        [this](const QString& code) {
            m_session.snapshotFailed(code);
            update();
        });
    m_deps.command.bind(
        CommandEvents{[this](const QByteArray& bytes) { m_session.commandOutput(bytes); },
                      [this](const qint64 bytes) { m_session.commandOutputLost(bytes); },
                      [this](const session::CommandExit& exit) {
                          m_session.commandFinished(exit);
                          update();
                      }});
    m_deps.command.onReadinessChanged([this] { update(); });
    m_deps.timers.onFired([this](const TimerKey key) { fired(key); });
}

RunnerLoop::~RunnerLoop() {
    m_deps.timers.onFired({});
    m_deps.command.onReadinessChanged({});
    m_queue.close();
}

// Start-up: the command runner's leftovers first, then the journal, and no acquire before both.
// The heartbeat starts at once and says draining until then; the Relay socket opens once the
// Relay has answered a heartbeat.
void RunnerLoop::start(std::function<void(ExitCode)> finished) {
    m_finished = std::move(finished);
    m_deps.environment.out(QStringLiteral("Runner %1 is running.").arg(m_deps.info.runnerId));
    m_deps.timers.arm(runTimer(RunTimer::StopPoll), stopPollMs);
    m_startup = Startup::CleaningUp;
    askHeartbeat();
    m_deps.timers.arm(runTimer(RunTimer::CleanupBound), startupCleanupBoundMs);
    m_deps.command.cleanUpLeftovers([this](const bool clean) { cleanupDone(clean, false); });
}

void RunnerLoop::cleanupDone(const bool clean, const bool bounded) {
    if (m_done)
        return;
    if (!bounded)
        m_cleanupClean = clean; // a late answer still counts
    if (m_startup != Startup::CleaningUp) {
        update();
        return;
    }
    m_deps.timers.disarm(runTimer(RunTimer::CleanupBound));
    if (bounded)
        m_deps.environment.err(QStringLiteral("The clean-up of the previous run did not end in "
                                              "%1 seconds; it stays unverified.")
                                   .arg(startupCleanupBoundMs / 1000));
    else if (!clean)
        m_deps.environment.err(QStringLiteral("The clean-up of the previous run could not be "
                                              "verified; this runner takes no job until it is."));
    restore();
}

void RunnerLoop::restore() {
    m_startup = Startup::Running;
    if (!m_deps.acceptedError.isEmpty())
        m_delivery.storageFailed(m_deps.acceptedError);
    const JobJournalFile::Loaded loaded = m_deps.journal.load();
    if (!loaded.ok) {
        m_delivery.storageFailed(loaded.error);
        m_delivery.restore(std::nullopt);
    } else {
        m_delivery.restore(loaded.record);
    }
    update();
}

bool RunnerLoop::holding() const {
    const delivery::JobDelivery::State state = m_delivery.state();
    return state != delivery::JobDelivery::State::Idle &&
           state != delivery::JobDelivery::State::Acquiring;
}

// Every cause in one place: any of them makes the runner draining, also while it holds a job.
bool RunnerLoop::draining() const {
    return m_startup != Startup::Running ||
           m_deps.command.readiness() != CommandHost::Readiness::Ready || !m_cleanupClean ||
           m_shuttingDown ||
           m_delivery.heartbeatFields().value(QStringLiteral("state")).toString() == drainingState;
}

QString RunnerLoop::reportedState() const {
    if (draining())
        return drainingState;
    return holding() ? busyState : idleState;
}

bool RunnerLoop::mayAcquire() const { return !draining() && !m_done && m_link->isReady(); }

// After every event that may change what the runner holds or reports.
void RunnerLoop::update() {
    if (m_done)
        return;
    releaseUnspecifiedLease();
    const bool nowHolding = holding();
    if (m_wasHolding && !nowHolding)
        m_deps.receiver.idle();
    m_wasHolding = nowHolding;
    if (m_shuttingDown && m_delivery.state() == delivery::JobDelivery::State::Idle) {
        m_deps.environment.out(QStringLiteral("Stopped."));
        finish(ExitCode::Ok);
        return;
    }
    if (!m_sentState.isEmpty() && reportedState() != m_sentState)
        heartbeatSoon();
}

// A draining runner gives back a lease whose spec it has not taken, whatever the cause.
void RunnerLoop::releaseUnspecifiedLease() {
    if (!draining() || m_delivery.state() != delivery::JobDelivery::State::Held ||
        m_session.phase() != session::JobSession::Phase::Assigned)
        return;
    if (m_sessionLink.withdraw())
        m_delivery.lease().release(QStringLiteral("draining"));
}

qint64 RunnerLoop::nowMs() const { return m_deps.timers.now().toMSecsSinceEpoch(); }

void RunnerLoop::askHeartbeat() {
    if (m_heartbeatAsked || m_done)
        return;
    m_heartbeatAsked = true;
    m_deps.timers.disarm(runTimer(RunTimer::Heartbeat));
    m_queue.heartbeat();
}

// A change of the reported state goes out at once, but never sooner than two seconds after the
// last heartbeat; a heartbeat that is failing keeps its backoff.
void RunnerLoop::heartbeatSoon() {
    if (m_heartbeatAsked || m_heartbeatFailing)
        return;
    const qint64 since = m_lastHeartbeatMs ? nowMs() - *m_lastHeartbeatMs : heartbeatSpacingMs;
    m_deps.timers.arm(runTimer(RunTimer::Heartbeat),
                      std::max<qint64>(0, heartbeatSpacingMs - since));
}

QJsonObject RunnerLoop::heartbeatBody() {
    m_sentState = reportedState();
    m_lastHeartbeatMs = nowMs();
    const QJsonObject fields = m_delivery.heartbeatFields();
    return {
        {QStringLiteral("state"), m_sentState},
        {QStringLiteral("labels"), QJsonArray::fromStringList(m_deps.heartbeatLabels)},
        {QStringLiteral("capabilities"), QJsonArray{QString(protocol::capability::runnerProtocol)}},
        {QStringLiteral("runnerVersion"), m_deps.environment.runnerVersion},
        {QStringLiteral("currentJob"), fields.value(QStringLiteral("currentJob"))},
    };
}

void RunnerLoop::heartbeatAnswered(const AccessTokens::Call& call) {
    m_heartbeatAsked = false;
    if (m_done)
        return;
    const AccessTokens::Outcome outcome = call.token.outcome;
    if (outcome == AccessTokens::Outcome::InvalidAccess ||
        outcome == AccessTokens::Outcome::RunnerRemoved ||
        outcome == AccessTokens::Outcome::StateFailure) {
        finish(reportTokenFailure(call.token, m_deps.environment));
        return;
    }
    if (call.relay.refused("runner_removed")) {
        tokenFailure(AccessTokens::Outcome::RunnerRemoved);
        return;
    }
    const QJsonValue disabled = call.relay.body.value(QStringLiteral("disabled"));
    if (outcome != AccessTokens::Outcome::Ok || !call.relay.ok() || !disabled.isBool()) {
        m_heartbeatFailing = true;
        m_deps.timers.arm(runTimer(RunTimer::Heartbeat), m_heartbeatBackoff.next());
        return;
    }
    m_heartbeatFailing = false;
    m_heartbeatBackoff.reset();
    if (!m_linkStarted) {
        m_linkStarted = true;
        m_link->start();
    }
    if (disabled.toBool() != m_disabled) {
        m_disabled = disabled.toBool();
        m_deps.environment.out(m_disabled ? QStringLiteral("This runner is disabled on the "
                                                           "desktop; it keeps reporting but "
                                                           "takes no job.")
                                          : QStringLiteral("This runner is enabled again."));
    }
    m_deps.timers.arm(runTimer(RunTimer::Heartbeat), heartbeatIntervalMs);
    const bool pending = call.relay.body.value(QStringLiteral("pending")).toBool();
    m_delivery.heartbeatAnswered(pending && mayAcquire(), m_disabled);
    m_queue.pairings();
    update();
}

void RunnerLoop::leaseAnswered(const delivery::LeaseCall& call,
                               const delivery::CallResult& result) {
    if (m_done)
        return;
    if (result.refused("runner_removed")) {
        tokenFailure(AccessTokens::Outcome::RunnerRemoved);
        return;
    }
    if (result.kind == delivery::CallResult::Kind::CredentialLost) {
        tokenFailure(AccessTokens::Outcome::InvalidAccess);
        return;
    }
    m_delivery.answered(call, result);
    update();
}

void RunnerLoop::tokenFailed(const AccessTokens::Outcome outcome) {
    finish(reportTokenFailure({outcome,
                               {},
                               QStringLiteral("The runner's credential could not "
                                              "be stored.")},
                              m_deps.environment));
}

void RunnerLoop::tokenFailure(const AccessTokens::Outcome outcome) {
    finish(reportTokenFailure({outcome, {}, {}}, m_deps.environment));
}

void RunnerLoop::pairingsChecked(const std::optional<ExitCode> exit) {
    if (exit)
        finish(*exit);
}

void RunnerLoop::checkStop() {
    const int stops = m_deps.stopSignals();
    if (stops >= 2) {
        m_deps.environment.err(QStringLiteral("Stopped at once by a second signal."));
        finish(ExitCode::Failure);
        return;
    }
    if (stops >= 1 && !m_shuttingDown)
        beginShutdown();
}

// No new job; a lease without a spec is given back, a started job runs to its end and its
// finish is answered before the process exits.
void RunnerLoop::beginShutdown() {
    m_shuttingDown = true;
    m_deps.environment.out(QStringLiteral("Stopping: no new job is taken."));
    m_delivery.drain();
    update();
}

void RunnerLoop::finish(const ExitCode code) {
    if (m_done)
        return;
    m_done = true;
    m_queue.close();
    m_link->stop();
    for (const RunTimer timer : {RunTimer::Heartbeat, RunTimer::CleanupBound, RunTimer::StopPoll})
        m_deps.timers.disarm(runTimer(timer));
    if (m_finished)
        m_finished(code);
}

} // namespace ketplus::runner::app
