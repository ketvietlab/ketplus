#pragma once

#include "ExitCode.h"
#include "delivery/DeliveryPorts.h"
#include "run/AccessTokens.h"
#include "session/SessionPorts.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

// What the run loop needs besides the job pieces: the Relay's HTTP routes, the command runner's
// readiness and clean-up, and timers. Production adapters and test fakes both implement these.
namespace ketplus::runner::app {

// The Relay's HTTP routes. Each call blocks until its answer; the RelayQueue makes sure only
// one runs at a time and that none starts from inside another.
class RelayPort {
  public:
    struct Token {
        AccessTokens::Outcome outcome = AccessTokens::Outcome::Retry;
        QString value; // Ok only; never logged
    };
    virtual ~RelayPort() = default;
    [[nodiscard]] virtual AccessTokens::Call heartbeat(const QJsonObject& body) = 0;
    [[nodiscard]] virtual delivery::CallResult lease(const delivery::LeaseCall& call) = 0;
    // An access token for the Relay socket; `renew` asks for a new one first.
    [[nodiscard]] virtual Token token(bool renew) = 0;
    // Checks every pending pin once; an exit code when one of them ends the run.
    [[nodiscard]] virtual std::optional<ExitCode> pairings() = 0;
};

// Where the command runner's events go.
struct CommandEvents {
    std::function<void(const QByteArray&)> output;
    std::function<void(qint64)> outputLost;
    std::function<void(const session::CommandExit&)> finished;
};

// The job's command runner, with what the loop needs before it takes a job.
class CommandHost {
  public:
    enum class Readiness { Unknown, Ready, Unavailable };
    using Changed = std::function<void()>;
    virtual ~CommandHost() = default;
    [[nodiscard]] virtual session::CommandRunner& runner() = 0;
    // Set once by the loop, before any launch.
    virtual void bind(CommandEvents events) = 0;
    [[nodiscard]] virtual Readiness readiness() const = 0;
    // Called whenever readiness() changes.
    virtual void onReadinessChanged(Changed changed) = 0;
    // Stops and removes what a previous run left. `done(clean)` once; clean is false when the
    // clean-up could not be confirmed.
    virtual void cleanUpLeftovers(std::function<void(bool clean)> done) = 0;
};

// One place for every timer of the run: the loop's own and the pieces'. Arming an armed timer
// restarts it.
enum class TimerDomain { Run, Session, Delivery, Peer };

struct TimerKey {
    TimerDomain domain = TimerDomain::Run;
    int id = 0;
    friend bool operator==(const TimerKey&, const TimerKey&) = default;
};

enum class RunTimer {
    Heartbeat,    // the next heartbeat
    CleanupBound, // the start-up clean-up gets this long before the run goes on without it
    StopPoll,     // reads the stop signals
};

class TimerHub {
  public:
    using Fired = std::function<void(TimerKey)>;
    virtual ~TimerHub() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
    virtual void arm(TimerKey key, qint64 delayMs) = 0;
    virtual void disarm(TimerKey key) = 0;
    // Where fired timers go; set once by the loop.
    virtual void onFired(Fired fired) = 0;
};

[[nodiscard]] inline TimerKey runTimer(const RunTimer timer) {
    return {TimerDomain::Run, static_cast<int>(timer)};
}

} // namespace ketplus::runner::app
