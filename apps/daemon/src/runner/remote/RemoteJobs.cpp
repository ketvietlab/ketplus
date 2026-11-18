#include "runner/remote/RemoteJobs.h"

#include "runner/sync/StoreRetention.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/BackgroundTaskOutput.h"

#include <QLoggingCategory>

namespace ketplus::daemon::runner::remote {

namespace {
Q_LOGGING_CATEGORY(lcRemoteJobs, "ketplus.runner.remote")

const QString runnerUnavailable = QStringLiteral("runner_unavailable");
const QString resourceExhausted = QStringLiteral("resource_exhausted");
const QString invalidState = QStringLiteral("invalid_state");
const QString invalidRequest = QStringLiteral("invalid_request");
const QString storageError = QStringLiteral("storage_error");
} // namespace

RemoteJobs::RemoteJobs(RemoteJobsPorts ports)
    : ports_(std::move(ports)), store_(ports_.database, *ports_.ledger),
      sessions_(records_, [this](const QString& jobId) { return makeSession(jobId); }),
      coordinator_(*ports_.relay, store_, *ports_.jobClock, *ports_.trust, sessions_),
      records_(coordinator_),
      snapshots_(
          specs_, [this](const QString& jobId) { return ownerOf(jobId); }, ports_.secret),
      snapshotPorts_(
          *ports_.stores,
          [this](const QString& jobId, const QString& runnerId) {
              return snapshots_.plan(jobId, runnerId);
          },
          [this](const QString& jobId, const QString& runnerId) {
              return acceptedBy(jobId, runnerId);
          },
          [this](const QString& jobId) {
              const auto found = coordinator_.recordOfJob(jobId);
              const peer::JobPeerSession* session = sessions_.session(jobId);
              return found && session && links_ && links_->bulkWritable(session->openConnection());
          },
          [this](const QString& jobId, const std::optional<QString>& code) {
              if (code)
                  sessions_.snapshotFailed(jobId, *code);
              else
                  sessions_.snapshotReady(jobId);
          },
          ports_.defer),
      cancels_(*ports_.relay, ports_.defer) {
    QString error;
    // The output store is otherwise created by the first local job that writes output.
    open_ = ports_.ledger && ports_.ledger->initialize(&error) &&
            BackgroundTaskOutput(ports_.database).initialize(&error);
    if (!open_)
        qCWarning(lcRemoteJobs) << "remote jobs unavailable: the registry could not be opened";
    coordinator_.setChanged([this](const jobs::RemoteJobRecord& record) { recorded(record); });
}

// A daemon that stops leaves the records as they are; the next start resumes them.
RemoteJobs::~RemoteJobs() {
    coordinator_.setChanged({});
    links_.reset();
}

bool RemoteJobs::available() const {
    return open_ && ports_.gitProblem.isEmpty() && ports_.relayAvailable && ports_.relayAvailable();
}

std::optional<jobs::RemoteJobRecord> RemoteJobs::record(const QString& taskId) const {
    return coordinator_.record(taskId);
}

// Cheap checks first; then the worktree is frozen off the event loop before the Relay hears of
// the job.
void RemoteJobs::submit(const QString& taskId, SubmitRequest request, Callback callback) {
    if (!available()) {
        callback(runnerUnavailable);
        return;
    }
    if (coordinator_.record(taskId) || submitting_.contains(taskId)) {
        callback(invalidState);
        return;
    }
    if (coordinator_.outstanding() + submitting_.size() >= jobs::maximumOutstanding) {
        callback(resourceExhausted);
        return;
    }
    if (!ports_.prepare || !ports_.stores) {
        callback(runnerUnavailable);
        return;
    }
    submitting_.insert(taskId, true);
    sync::PrepareRequest prepare;
    prepare.cwd = request.spec.cwd;
    prepare.storesRoot = ports_.stores->storesRoot();
    prepare.inFlightRef = sync::StoreRetention::inFlightRef(taskId);
    const std::weak_ptr<int> alive = alive_;
    ports_.prepare(prepare, [this, alive, taskId, request = std::move(request),
                             callback = std::move(callback)](sync::PreparedOutcome outcome) {
        if (alive.expired())
            return;
        submitting_.remove(taskId);
        prepared(taskId, request, outcome, callback);
    });
}

void RemoteJobs::prepared(const QString& taskId, const SubmitRequest& request,
                          const sync::PreparedOutcome& outcome, const Callback& callback) {
    if (!outcome.prepared) {
        callback(outcome.code.isEmpty() ? sync::prepare::snapshotBuildFailed : outcome.code);
        return;
    }
    const sync::Prepared& prepared = *outcome.prepared;
    // Nothing of a submit that went no further is left behind: no spec, ref or pack.
    const auto undo = [&](const QString& code) {
        specs_.forget(taskId);
        snapshots_.remove(taskId);
        ports_.stores->release(prepared.storePath, taskId);
        callback(code);
    };
    if (!available()) {
        undo(runnerUnavailable);
        return;
    }
    peer::protocol::JobSpec spec = request.spec;
    spec.cwd = prepared.cwd;
    spec.snapshotId = prepared.snapshotId;
    spec.sourceHead = prepared.sourceHead;
    spec.objectFormat = prepared.objectFormat;
    spec.timeoutMs = request.timeoutMs;
    if (!specs_.store(taskId, spec)) {
        undo(invalidRequest);
        return;
    }
    snapshots_.put(taskId,
                   TaskSnapshot{prepared.repositoryId, prepared.worktreeId, prepared.storePath,
                                prepared.snapshotId, prepared.warnings});
    const jobs::JobDecision decision =
        coordinator_.start(taskId, request.runsOn, request.timeoutMs,
                           jobs::RemoteSnapshot{prepared.repositoryId, prepared.worktreeId,
                                                prepared.snapshotId, prepared.warnings});
    if (!decision.ok()) {
        if (!coordinator_.record(taskId))
            undo(decision.code);
        else
            callback(decision.code);
        return;
    }
    qCInfo(lcRemoteJobs).noquote() << "remote job submitted task=" + taskId;
    callback({});
}

jobs::JobDecision RemoteJobs::stop(const QString& taskId) { return coordinator_.stop(taskId); }

jobs::JobDecision RemoteJobs::resolveCleanup(const QString& taskId, const QString& userClientId) {
    const auto current = coordinator_.record(taskId);
    if (!current)
        return {QStringLiteral("not_found")};
    if (current->state != jobs::state::needsAttention)
        return {invalidState};
    if (userClientId.isEmpty() || !store_.acknowledge(taskId, QStringLiteral("user"), userClientId))
        return {invalidRequest};
    return coordinator_.resolveCleanup(taskId);
}

jobs::JobDecision RemoteJobs::runnerRemoved(const QString& runnerId, const QString& clientId) {
    const QString client = clientId.isEmpty() ? QStringLiteral("daemon") : clientId;
    // The records the coordinator ends for this runner leave needs_attention acknowledged.
    for (const QString& taskId : std::as_const(live_)) {
        const auto current = coordinator_.record(taskId);
        if (current && !current->terminal() && current->dispatchedTo == runnerId)
            (void)store_.acknowledge(taskId, QStringLiteral("runner_removed"), client);
    }
    const jobs::JobDecision decision = coordinator_.runnerRemoved(runnerId);
    ports_.stores->releaseRunner(runnerId);
    return decision;
}

void RemoteJobs::jobTimer(const QString& taskId, const jobs::JobTimer timer) {
    coordinator_.timerFired(taskId, timer);
}

void RemoteJobs::peerTimer(const QString& jobId, const peer::PeerTimer timer) {
    sessions_.timerFired(jobId, timer);
}

void RemoteJobs::sweep(const QDateTime& now) {
    if (ports_.stores)
        ports_.stores->sweep(now);
}

// Every record the coordinator committed.
void RemoteJobs::recorded(const jobs::RemoteJobRecord& record) {
    specs_.recorded(record);
    if (!record.terminal())
        live_.insert(record.taskId);
    else
        live_.remove(record.taskId);
    if (record.terminal()) {
        // The session or sender that reported this may still be on the stack.
        const std::weak_ptr<int> alive = alive_;
        ports_.defer(0, [this, alive, record] {
            if (!alive.expired())
                ended(record);
        });
    }
    if (changed_)
        changed_(record);
}

void RemoteJobs::ended(const jobs::RemoteJobRecord& record) {
    if (!record.jobId.isEmpty()) {
        sessions_.release(record.jobId);
        snapshotPorts_.ended(record.jobId);
        spools_.remove(record.jobId);
    }
    if (const auto snapshot = snapshots_.find(record.taskId))
        ports_.stores->release(snapshot->storePath, record.taskId);
    snapshots_.remove(record.taskId);
}

std::optional<TaskSnapshots::JobOwner> RemoteJobs::ownerOf(const QString& jobId) const {
    const auto found = coordinator_.recordOfJob(jobId);
    if (!found)
        return std::nullopt;
    const QString runnerId =
        found->dispatched() ? found->dispatchedTo : (found->runner ? found->runner->runnerId : "");
    return TaskSnapshots::JobOwner{found->taskId, runnerId};
}

// The snapshot goes out only once job.accepted is durable; a lost write is tried once more.
bool RemoteJobs::acceptedBy(const QString& jobId, const QString& runnerId) {
    const auto accepted = [&] {
        const auto found = coordinator_.recordOfJob(jobId);
        return found && found->dispatchedTo == runnerId &&
               (found->placement == jobs::placement::syncing ||
                found->placement == jobs::placement::running);
    };
    if (accepted())
        return true;
    coordinator_.accepted(jobId, runnerId);
    return accepted();
}

std::unique_ptr<peer::JobPeerSession> RemoteJobs::makeSession(const QString& jobId) {
    const auto spool = spools_.constFind(jobId);
    if (spool == spools_.cend() || !ports_.peerClock)
        return nullptr;
    return std::make_unique<peer::JobPeerSession>(
        jobId, ports_.daemonVersion,
        peer::SessionPorts{currentLinks_, *ports_.peerClock, records_, snapshots_, snapshotPorts_,
                           **spool});
}

} // namespace ketplus::daemon::runner::remote
