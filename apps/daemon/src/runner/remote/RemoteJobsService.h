#pragma once

#include "storage/BackgroundTaskLedger.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

namespace ketplus::daemon {
class CloudControlClient;
}

namespace ketplus::daemon::runner {
class RunnerPinStore;
class RunnerRelayPort;
namespace jobs {
class PinRunnerTrust;
class QtJobClock;
class RemoteJobRelay;
} // namespace jobs
namespace link {
class FunctionMachineIdentity;
class FunctionRunnerPins;
class QtLinkClock;
class WebRtcAnswerTransportFactory;
} // namespace link
namespace peer {
class QtJobPeerClock;
}
namespace entry {
class RemoteJobEntry;
} // namespace entry
namespace sync {
class RunnerSecret;
}
} // namespace ketplus::daemon::runner

// Remote jobs on the running daemon's real parts: the background task registry, the Control
// client's runner port and Relay socket, the pin store, WebRTC and the system git.
namespace ketplus::daemon::runner::remote {

class RemoteJobs;
class SnapshotStores;

// The Relay socket as remote jobs use it.
class RelaySocket {
  public:
    using Take = std::function<bool(const QJsonObject& message)>;
    virtual ~RelaySocket() = default;
    virtual bool send(const QJsonObject& message) = 0;
    // Offers each message to `take` first and reports each opening; empty gives them back.
    virtual void take(Take take, std::function<void()> reopened) = 0;
};

class RemoteJobsService final : public QObject {
    Q_OBJECT
  public:
    static constexpr int hourlySweepMs = 60 * 60 * 1000;

    // `ledger` is a handle on the daemon's registry; `stateDirectory` holds the snapshot stores.
    // Runners the user removes through the Control client end their jobs.
    RemoteJobsService(BackgroundTaskLedger ledger, CloudControlClient& cloud,
                      const QString& stateDirectory, QObject* parent = nullptr);
    // The same on any runner port and socket, sweeping every `sweepIntervalMs`.
    RemoteJobsService(BackgroundTaskLedger ledger, RunnerRelayPort& port,
                      std::unique_ptr<RelaySocket> socket, const QString& stateDirectory,
                      int sweepIntervalMs = hourlySweepMs, QObject* parent = nullptr);
    ~RemoteJobsService() override;

    // Once, before the Control client starts: resumes the records and takes runner messages.
    void start();
    [[nodiscard]] RemoteJobs& jobs() { return *jobs_; }
    // Where background tasks started with runs_on meet the remote jobs.
    [[nodiscard]] entry::RemoteJobEntry& entry() { return *entry_; }

  private:
    class Signals;

    BackgroundTaskLedger ledger_;
    std::unique_ptr<RelaySocket> socket_;
    std::unique_ptr<RunnerPinStore> pins_;
    std::unique_ptr<jobs::RemoteJobRelay> relay_;
    std::unique_ptr<jobs::QtJobClock> jobClock_;
    std::unique_ptr<jobs::PinRunnerTrust> trust_;
    std::unique_ptr<peer::QtJobPeerClock> peerClock_;
    std::unique_ptr<SnapshotStores> stores_;
    std::unique_ptr<sync::RunnerSecret> secret_;
    std::unique_ptr<link::QtLinkClock> linkClock_;
    std::unique_ptr<link::WebRtcAnswerTransportFactory> transports_;
    std::unique_ptr<link::FunctionRunnerPins> linkPins_;
    std::unique_ptr<link::FunctionMachineIdentity> identity_; // of the current links
    std::unique_ptr<Signals> signals_;
    QTimer sweep_;
    // Last, so it goes first: it holds references to every part above.
    std::unique_ptr<RemoteJobs> jobs_;
    std::unique_ptr<entry::RemoteJobEntry> entry_;
};

} // namespace ketplus::daemon::runner::remote
