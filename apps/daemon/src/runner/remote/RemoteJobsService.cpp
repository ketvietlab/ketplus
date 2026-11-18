#include "runner/remote/RemoteJobsService.h"

#include "cloud/CloudControlClient.h"
#include "runner/RunnerPinStore.h"
#include "runner/RunnerService.h"
#include "runner/jobs/PinRunnerTrust.h"
#include "runner/jobs/QtJobClock.h"
#include "runner/jobs/RemoteJobRelay.h"
#include "runner/link/LinkAdapters.h"
#include "runner/output/TaskOutputSpool.h"
#include "runner/peer/QtJobPeerClock.h"
#include "runner/remote/RemoteJobs.h"
#include "runner/remote/SnapshotStores.h"
#include "runner/sync/SnapshotKeys.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QPointer>
#include <QThreadPool>

#include <algorithm>

namespace ketplus::daemon::runner::remote {

namespace {
constexpr int sweepIntervalMs = 60 * 60 * 1000;
} // namespace

// Signaling of the runner links leaves on the Relay socket.
class RemoteJobsService::Signals final : public link::SignalOut {
  public:
    explicit Signals(CloudControlClient& cloud) : cloud_(cloud) {}
    void sendSignal(const QJsonObject& message) override { (void)cloud_.sendRelayMessage(message); }

  private:
    CloudControlClient& cloud_;
};

RemoteJobsService::RemoteJobsService(BackgroundTaskLedger ledger, CloudControlClient& cloud,
                                     const QString& stateDirectory, QObject* parent)
    : QObject(parent), ledger_(std::move(ledger)), cloud_(cloud) {
    RunnerRelayPort& port = cloud_.runners().port();
    pins_ = std::make_unique<RunnerPinStore>(port.credentials());
    relay_ = std::make_unique<jobs::RemoteJobRelay>(port);
    jobClock_ = std::make_unique<jobs::QtJobClock>();
    trust_ = std::make_unique<jobs::PinRunnerTrust>(*pins_);
    peerClock_ = std::make_unique<peer::QtJobPeerClock>(
        [this](const QString& jobId, const peer::PeerTimer timer) {
            jobs_->peerTimer(jobId, timer);
        });
    const QString root = QDir(stateDirectory).filePath(QStringLiteral("runner-snapshots"));
    stores_ = std::make_unique<SnapshotStores>(QDir(root).filePath(QStringLiteral("stores")),
                                               QDir(root).filePath(QStringLiteral("packs")));
    secret_ = std::make_unique<sync::RunnerSecret>(port.credentials());
    linkClock_ = std::make_unique<link::QtLinkClock>();
    transports_ = std::make_unique<link::WebRtcAnswerTransportFactory>();
    linkPins_ = std::make_unique<link::FunctionRunnerPins>(
        [pins = pins_.get()](const QString& runnerId) { return pins->pin(runnerId); });
    signals_ = std::make_unique<Signals>(cloud_);

    const QPointer<RemoteJobsService> self(this);
    const auto defer = [self](const qint64 delayMs, std::function<void()> work) {
        if (self)
            QTimer::singleShot(std::max<qint64>(0, delayMs), self.data(), std::move(work));
    };
    RemoteJobsPorts ports;
    ports.database = ledger_.database();
    ports.ledger = &ledger_;
    ports.relay = relay_.get();
    ports.jobClock = jobClock_.get();
    ports.trust = trust_.get();
    ports.peerClock = peerClock_.get();
    ports.stores = stores_.get();
    ports.daemonVersion = QCoreApplication::applicationVersion();
    ports.relayAvailable = [&port] { return port.runnerRelayAvailable(); };
    ports.accountScope = [&port] { return port.accountScope(); };
    ports.gitProblem = sync::checkGit();
    ports.secret = [secret = secret_.get()] { return secret->obtain(); };
    // Git runs on the shared pool; the answer comes back on this object's thread.
    ports.prepare = [self](sync::PrepareRequest request,
                           std::function<void(sync::PreparedOutcome)> done) {
        QThreadPool::globalInstance()->start(
            [self, request = std::move(request), done = std::move(done)]() mutable {
                sync::PreparedOutcome outcome = sync::prepareSnapshot(request);
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [self, outcome = std::move(outcome), done = std::move(done)] {
                        if (self)
                            done(outcome);
                    },
                    Qt::QueuedConnection);
            });
    };
    ports.makeLinks = [this,
                       &port](link::LinkEvents& events) -> std::unique_ptr<link::RunnerLinks> {
        const std::optional<MachineIdentity> identity = port.machineIdentity();
        if (!identity)
            return nullptr;
        identity_ = std::make_unique<link::FunctionMachineIdentity>(
            identity->workspaceId, identity->machineId, identity->keyVersion,
            [&port](const QByteArray& bytes) { return port.signAsMachine(bytes); });
        return std::make_unique<link::RunnerLinks>(*signals_, *linkClock_, *transports_, *identity_,
                                                   *linkPins_, events);
    };
    ports.openSpool = [database = ledger_.database()](
                          const QString& taskId) -> std::unique_ptr<peer::OutputSpool> {
        return output::TaskOutputSpool::open(database, taskId);
    };
    ports.defer = defer;
    jobs_ = std::make_unique<RemoteJobs>(std::move(ports));

    jobClock_->setFired([this](const QString& taskId, const jobs::JobTimer timer) {
        jobs_->jobTimer(taskId, timer);
    });
    linkClock_->setFired([this](const QString& connectionId) { jobs_->linkTimer(connectionId); });
    sweep_.setObjectName(QStringLiteral("remote-jobs-sweep"));
    sweep_.setInterval(sweepIntervalMs);
    connect(&sweep_, &QTimer::timeout, this,
            [this] { jobs_->sweep(QDateTime::currentDateTimeUtc()); });
    connect(&cloud_.runners(), &RunnerService::runnerRemoved, this,
            [this](const QString& runnerId, const QString& clientId) {
                (void)jobs_->runnerRemoved(runnerId, clientId);
            });
}

RemoteJobsService::~RemoteJobsService() {
    cloud_.setRunnerMessages({}, {});
    sweep_.stop();
    jobClock_->setFired({});
    linkClock_->setFired({});
    jobs_.reset();
}

void RemoteJobsService::start() {
    jobs_->restore();
    cloud_.setRunnerMessages(
        [this](const QJsonObject& message) { return jobs_->relayMessage(message); },
        [this] { jobs_->relaySocketReopened(); });
    jobs_->sweep(QDateTime::currentDateTimeUtc());
    sweep_.start();
}

} // namespace ketplus::daemon::runner::remote
