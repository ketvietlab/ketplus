#include "run/RunJobs.h"

#include "Environment.h"
#include "peer/QtRelaySocket.h"
#include "peer/WebRtcTransport.h"
#include "run/RunClocks.h"
#include "run/RunnerAssembly.h"
#include "run/RunnerLoop.h"

#include <QEventLoop>

#include <atomic>
#include <csignal>

namespace ketplus::runner::app {
namespace {

std::atomic_int stopSignalCount{0};

extern "C" void countStop(int) { stopSignalCount.fetch_add(1); }

// SIGTERM and SIGINT count the same while the loop runs; the handler only counts. The previous
// handlers come back after.
class StopSignals {
  public:
    StopSignals() {
        stopSignalCount.store(0);
        struct sigaction action{};
        action.sa_handler = countStop;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, &m_term);
        sigaction(SIGINT, &action, &m_int);
    }
    ~StopSignals() {
        sigaction(SIGTERM, &m_term, nullptr);
        sigaction(SIGINT, &m_int, nullptr);
    }
    StopSignals(const StopSignals&) = delete;
    StopSignals& operator=(const StopSignals&) = delete;

  private:
    struct sigaction m_term{};
    struct sigaction m_int{};
};

} // namespace

ExitCode runJobs(PairingContext& context, const RunnerIdentity& identity,
                 const StateDirectory& directory, const QString& workDirOption) {
    const Environment& environment = context.environment;
    const StopSignals stopSignals;
    QtTimerHub timers;
    peer::WebRtcTransportFactory transports;
    RunSeams seams{
        timers,
        [&environment](peer::RelaySocketEvents& events) {
            return std::make_unique<peer::QtRelaySocket>(environment.relayUrl, events);
        },
        transports,
        [&environment] {
            return std::max(stopSignalCount.load(), environment.stopRequested() ? 1 : 0);
        },
    };
    ExitCode failure = ExitCode::Failure;
    const std::unique_ptr<RunnerAssembly> assembly = RunnerAssembly::create(
        context, identity, directory, workDirOption, std::move(seams), &failure);
    if (!assembly)
        return failure;

    QEventLoop events;
    std::optional<ExitCode> exit;
    assembly->loop().start([&](const ExitCode code) {
        exit = code;
        events.exit();
    });
    if (!exit)
        events.exec();
    return exit.value_or(ExitCode::Failure);
}

} // namespace ketplus::runner::app
