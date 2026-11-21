#pragma once

#include "ExitCode.h"
#include "exec/ExecPorts.h"
#include "flow/PendingPins.h"
#include "peer/PeerPorts.h"
#include "run/RunPorts.h"
#include "state/RunnerState.h"
#include "state/StateDirectory.h"

#include <functional>
#include <memory>

// The production wiring of `run`: the runner's state, work directory, Relay, executor and job
// pieces around one RunnerLoop. Only time, the Relay socket, the peer transport and the stop
// signals come from outside, so tests drive this same wiring without real time or network.
namespace ketplus::runner::app {

class RunnerLoop;

struct RunSeams {
    TimerHub& timers;
    std::function<std::unique_ptr<peer::RelaySocket>(peer::RelaySocketEvents&)> socket;
    peer::PeerTransportFactory& transports;
    std::function<int()> stopSignals; // SIGTERM and SIGINT received so far
};

class RunnerAssembly {
  public:
    // Nothing, with the exit code in `failure`, when the work directory or the snapshot store
    // cannot be opened.
    [[nodiscard]] static std::unique_ptr<RunnerAssembly>
    create(PairingContext& context, const RunnerIdentity& identity, const StateDirectory& directory,
           const QString& workDirOption, RunSeams seams, ExitCode* failure);
    ~RunnerAssembly();
    RunnerAssembly(const RunnerAssembly&) = delete;
    RunnerAssembly& operator=(const RunnerAssembly&) = delete;

    [[nodiscard]] RunnerLoop& loop();
    [[nodiscard]] const QString& workDirectory() const;

  private:
    struct Parts;
    explicit RunnerAssembly(std::unique_ptr<Parts> parts);
    std::unique_ptr<Parts> m_parts;
};

// What the commands of a job inherit from the runner: PATH, HOME, LANG, USER and LOGNAME, each
// when set. The spec's variables override them.
[[nodiscard]] QMap<QString, QString> baseEnvironment(const Environment& environment);

} // namespace ketplus::runner::app
