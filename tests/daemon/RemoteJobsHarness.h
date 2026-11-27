#pragma once

#include "PeerSessionFakes.h"
#include "RemoteJobLedgerFixture.h"
#include "RemoteJobTestFakes.h"
#include "runner/remote/RemoteJobs.h"
#include "runner/remote/SnapshotStores.h"

#include <QTemporaryDir>

#include <functional>
#include <memory>
#include <optional>

// RemoteJobs on a registry in a temporary SQLite file, a Relay the test answers, test clocks
// and real snapshot stores in a temporary directory. Deferred work runs when the test says.
namespace remote_jobs_test {

using namespace remote_job_ledger_test;
using namespace ketplus::daemon::runner::remote;
namespace sync = ketplus::daemon::runner::sync;

struct RemoteHarness {
    LedgerFixture f;
    remote_job_test::FakeRelay relay;
    remote_job_test::FakeClock clock;
    remote_job_test::FakeTrust trust;
    peer_session_test::FakeClock peerClock;
    QTemporaryDir state;
    std::unique_ptr<SnapshotStores> stores;
    struct Deferred {
        qint64 delayMs = 0;
        std::function<void()> work;
    };
    QList<Deferred> deferred;
    std::function<sync::PreparedOutcome(const sync::PrepareRequest&)> prepare;
    QList<sync::PrepareRequest> prepared; // requests whose answer the test holds
    QList<std::function<void(sync::PreparedOutcome)>> preparing;
    QString scope = QStringLiteral("account-1");
    // Changes the ports before each build, as for real links or spools.
    std::function<void(RemoteJobsPorts&)> adjust;
    std::unique_ptr<RemoteJobs> jobs;
    // Another registry to run on, as a session host's journal holds; the fixture's otherwise.
    struct Registry {
        QSqlDatabase database;
        ketplus::daemon::BackgroundTaskLedger* ledger = nullptr;
    };
    std::optional<Registry> registry;

    RemoteHarness() { build(); }

    [[nodiscard]] QSqlDatabase database() const {
        return registry ? registry->database : f.database;
    }

    // A new RemoteJobs on the same registry, as after a daemon restart.
    void build() {
        jobs.reset();
        deferred.clear();
        stores =
            std::make_unique<SnapshotStores>(state.filePath("stores"), state.filePath("packs"));
        RemoteJobsPorts ports;
        ports.database = database();
        ports.ledger = registry ? registry->ledger : f.ledger.get();
        ports.relay = &relay;
        ports.jobClock = &clock;
        ports.trust = &trust;
        ports.peerClock = &peerClock;
        ports.stores = stores.get();
        ports.daemonVersion = QStringLiteral("1.0.0");
        ports.relayAvailable = [this] { return relay.offered; };
        ports.accountScope = [this] { return scope; };
        ports.secret = [] { return std::optional<QByteArray>(QByteArray(32, 's')); };
        ports.prepare = [this](sync::PrepareRequest request,
                               std::function<void(sync::PreparedOutcome)> done) {
            if (prepare) {
                done(prepare(request));
                return;
            }
            prepared.append(request);
            preparing.append(std::move(done));
        };
        ports.makeLinks = [](ketplus::daemon::runner::link::LinkEvents&) { return nullptr; };
        ports.openSpool = [](const QString&) { return nullptr; };
        ports.defer = [this](const qint64 delayMs, std::function<void()> work) {
            deferred.append({delayMs, std::move(work)});
        };
        if (adjust)
            adjust(ports);
        jobs = std::make_unique<RemoteJobs>(std::move(ports));
    }

    // Runs deferred work due within `delayMs`, also work it defers in turn.
    int run(const qint64 delayMs = 0) {
        int ran = 0;
        for (bool found = true; found;) {
            found = false;
            for (qsizetype i = 0; i < deferred.size(); ++i) {
                if (deferred.at(i).delayMs > delayMs)
                    continue;
                const Deferred next = deferred.takeAt(i);
                next.work();
                ++ran;
                found = true;
                break;
            }
        }
        return ran;
    }

    // Writes `steps` in order as the task's records, from its first one.
    bool write(const QList<RemoteJobRecord>& steps) {
        qint64 revision = 0;
        for (const RemoteJobRecord& step : steps) {
            if (!f.store->put(step, revision))
                return false;
            ++revision;
        }
        return true;
    }
};

inline RemoteJobRecord recordOf(const QString& taskId) {
    RemoteJobRecord record;
    record.taskId = taskId;
    record.runsOn = linuxLabels;
    record.timeoutMs = 60'000;
    return record;
}

// queued in the Relay with `jobId`.
inline QList<RemoteJobRecord> queuedSteps(const QString& taskId, const QString& jobId) {
    RemoteJobRecord first = recordOf(taskId);
    RemoteJobRecord queued = first;
    queued.placement = placement::relayQueued;
    queued.jobId = jobId;
    return {first, queued};
}

// Assigned to `runnerId`, its spec dispatched, accepted: syncing.
inline QList<RemoteJobRecord> syncingSteps(const QString& taskId, const QString& jobId,
                                           const QString& runnerId,
                                           const std::optional<RemoteSnapshot>& snapshot) {
    QList<RemoteJobRecord> steps = queuedSteps(taskId, jobId);
    for (RemoteJobRecord& step : steps)
        step.snapshot = snapshot;
    RemoteJobRecord assigned = steps.last();
    assigned.state = state::starting;
    assigned.placement = placement::assigned;
    assigned.runner = RemoteRunner{runnerId, QStringLiteral("Runner"), 1};
    RemoteJobRecord dispatched = assigned;
    dispatched.dispatchedTo = runnerId;
    RemoteJobRecord syncing = dispatched;
    syncing.placement = placement::syncing;
    steps << assigned << dispatched << syncing;
    return steps;
}

} // namespace remote_jobs_test
