#pragma once

#include "storage/BackgroundTaskLedger.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace ketplus::daemon {
class CloudControlClient;
}

namespace ketplus::daemon::runner {
class RunnerPinStore;
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
namespace sync {
class RunnerSecret;
}
} // namespace ketplus::daemon::runner

// Remote jobs on the running daemon's real parts: the background task registry, the Control
// client's runner port and Relay socket, the pin store, WebRTC and the system git.
namespace ketplus::daemon::runner::remote {

class RemoteJobs;
class SnapshotStores;

class RemoteJobsService final : public QObject {
    Q_OBJECT
  public:
    // `ledger` is a handle on the daemon's registry; `stateDirectory` holds the snapshot stores.
    RemoteJobsService(BackgroundTaskLedger ledger, CloudControlClient& cloud,
                      const QString& stateDirectory, QObject* parent = nullptr);
    ~RemoteJobsService() override;

    // Once, before the Control client starts: resumes the records and takes runner messages.
    void start();
    [[nodiscard]] RemoteJobs& jobs() { return *jobs_; }

  private:
    class Signals;

    BackgroundTaskLedger ledger_;
    CloudControlClient& cloud_;
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
    QTimer sweep_{this}; // named "remote-jobs-sweep"
    // Last, so it goes first: it holds references to every part above.
    std::unique_ptr<RemoteJobs> jobs_;
};

} // namespace ketplus::daemon::runner::remote
