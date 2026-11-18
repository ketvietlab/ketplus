// Submitting through RemoteJobs: the checks before the worktree is frozen, what a failed or
// refused submit leaves behind, and a runner the user removed.
#include "RemoteJobsHarness.h"
#include "runner/jobs/RemoteJobLabels.h"
#include "runner/sync/StoreRetention.h"

#include <QSqlQuery>
#include <QTest>

using namespace remote_jobs_test;
using remote_job_test::relayJob;
using remote_job_test::runnerA;

namespace {

SubmitRequest request() {
    SubmitRequest submit;
    submit.spec.cwd = QStringLiteral("/work/repo");
    submit.runsOn = linuxLabels;
    submit.timeoutMs = 60'000;
    return submit;
}

sync::PreparedOutcome preparedIn(const RemoteHarness& h) {
    sync::Prepared prepared;
    prepared.worktreeRoot = QStringLiteral("/work/repo");
    prepared.repositoryId = QStringLiteral("/work/repo/.git");
    prepared.worktreeId = QStringLiteral("/work/repo");
    prepared.storePath = sync::storePathFor(h.stores->storesRoot(), prepared.repositoryId);
    prepared.snapshotId = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    prepared.objectFormat = QStringLiteral("sha1");
    return {prepared, {}};
}

QStringList capturedLog;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedLog.append(message);
}

} // namespace

class RemoteJobsSubmitTest final : public QObject {
    Q_OBJECT

  private slots:
    void aSubmitKeepsTheSnapshotInItsFirstRecord() {
        RemoteHarness h;
        h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(h); };
        const QString id = h.f.remoteTask();
        QString code = QStringLiteral("unanswered");
        h.jobs->submit(id, request(), [&](const QString& answer) { code = answer; });
        QCOMPARE(code, QString());
        const auto record = h.jobs->record(id);
        QVERIFY(record);
        QVERIFY(record->snapshot);
        QCOMPARE(record->snapshot->snapshotId, preparedIn(h).prepared->snapshotId);
        QCOMPARE(h.relay.pending(QStringLiteral("submit")), 1);
        QCOMPARE(h.f.store->load().first().snapshot, record->snapshot);
    }

    // The same labels in another order: the same task and job, not an idempotency conflict.
    void theSameLabelsInAnotherOrderAreTheSameJob() {
        RemoteHarness h;
        h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(h); };
        const QStringList one = {QStringLiteral("os:linux"), QStringLiteral("arch:x86_64"),
                                 QStringLiteral("gpu")};
        const QStringList other = {QStringLiteral("gpu"), QStringLiteral("os:linux"),
                                   QStringLiteral("arch:x86_64")};
        const QString id =
            taskIdOf(h.f.ledger->create(intentFor(QStringLiteral("same"), normalizedRunsOn(one))));
        QVERIFY(!id.isEmpty());
        QCOMPARE(taskIdOf(h.f.ledger->create(
                     intentFor(QStringLiteral("same"), normalizedRunsOn(other)))),
                 id);
        // Unnormalized, the other order is another spec.
        QCOMPARE(h.f.ledger->create(intentFor(QStringLiteral("same"), other))
                     .value("error_code")
                     .toString(),
                 QStringLiteral("idempotency_conflict"));
        QVERIFY(h.f.approve(id));

        SubmitRequest submit = request();
        submit.runsOn = other;
        QString code = QStringLiteral("unanswered");
        h.jobs->submit(id, submit, [&](const QString& answer) { code = answer; });
        QCOMPARE(code, QString());
        QCOMPARE(h.jobs->record(id)->runsOn, normalizedRunsOn(one));
        QCOMPARE(h.relay.calls.last().runsOn, normalizedRunsOn(one));
    }

    // Logs name tasks and jobs only: no worktree, repository or store path, no snapshot id.
    void logsCarryNoPath() {
        capturedLog.clear();
        const auto previous = qInstallMessageHandler(captureLog);
        {
            RemoteHarness h;
            h.prepare = [&](const sync::PrepareRequest&) { return preparedIn(h); };
            const QString id = h.f.remoteTask();
            h.jobs->submit(id, request(), [](const QString&) {});
            QVERIFY(
                h.f.exec("UPDATE remote_jobs SET record='{not json' WHERE task_id='" + id + "'"));
            h.build();
            h.jobs->restore();
            const QString kept = h.f.remoteTask();
            QVERIFY(h.write(syncingSteps(kept, QStringLiteral("job-2"), runnerA,
                                         RemoteSnapshot{QStringLiteral("/work/repo/.git"),
                                                        QStringLiteral("/work/repo"),
                                                        QStringLiteral("feedface"),
                                                        {}})));
            h.build();
            h.jobs->restore();
            (void)h.jobs->runnerRemoved(runnerA, {});
        }
        qInstallMessageHandler(previous);
        QVERIFY(!capturedLog.isEmpty());
        for (const QString& line : std::as_const(capturedLog)) {
            QVERIFY2(!line.contains(QStringLiteral("/work/repo")), qPrintable(line));
            QVERIFY2(!line.contains(QStringLiteral("feedface")), qPrintable(line));
            QVERIFY2(!line.contains(QStringLiteral("0123456789abcdef0123456789abcdef01234567")),
                     qPrintable(line));
        }
    }

    void aFailedPrepareLeavesNothingAndTellsNoRelay() {
        RemoteHarness h;
        h.prepare = [](const sync::PrepareRequest&) {
            return sync::PreparedOutcome{std::nullopt, sync::prepare::snapshotBuildFailed};
        };
        const QString id = h.f.remoteTask();
        QString code;
        h.jobs->submit(id, request(), [&](const QString& answer) { code = answer; });
        QCOMPARE(code, sync::prepare::snapshotBuildFailed);
        QVERIFY(!h.jobs->record(id));
        QVERIFY(h.relay.log.isEmpty());
        QCOMPARE(h.f.count("SELECT count(*) FROM remote_jobs WHERE revision>0"), 0);
    }

    void aSubmitThatTheRelayStopsOfferingEndsBeforeTheRecord() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        QString code = QStringLiteral("unanswered");
        h.jobs->submit(id, request(), [&](const QString& answer) { code = answer; });
        QCOMPARE(h.prepared.size(), 1);
        QCOMPARE(h.prepared.first().inFlightRef,
                 sync::StoreRetention::inFlightRef(id)); // held for this task only
        h.relay.offered = false;
        h.preparing.takeFirst()(preparedIn(h));
        QCOMPARE(code, QStringLiteral("runner_unavailable"));
        QVERIFY(!h.jobs->record(id));
        QVERIFY(h.relay.log.isEmpty());
    }

    void theSameTaskIsNotSubmittedTwice() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        h.jobs->submit(id, request(), [](const QString&) {});
        QString code;
        h.jobs->submit(id, request(), [&](const QString& answer) { code = answer; });
        QCOMPARE(code, QStringLiteral("invalid_state"));
        QCOMPARE(h.prepared.size(), 1);
    }

    // The limit counts submits still freezing their worktree, and is checked before freezing.
    void atMostSixtyFourJobsAreOutstanding() {
        RemoteHarness h;
        for (int i = 0; i < 64; ++i) {
            QString code = QStringLiteral("unanswered");
            h.jobs->submit(h.f.remoteTask(), request(),
                           [&](const QString& answer) { code = answer; });
            QCOMPARE(code, QStringLiteral("unanswered"));
        }
        QString code;
        h.jobs->submit(h.f.remoteTask(), request(), [&](const QString& answer) { code = answer; });
        QCOMPARE(code, QStringLiteral("resource_exhausted"));
        QCOMPARE(h.prepared.size(), 64);
    }

    // The record of a removed runner ends acknowledged by the daemon when no local client asked.
    void aRemovedRunnerEndsItsRecordsAcknowledgedByTheDaemon() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        QVERIFY(h.write(syncingSteps(id, QStringLiteral("job-1"), runnerA, std::nullopt)));
        h.build();
        h.jobs->restore();
        QVERIFY(h.jobs->runnerRemoved(runnerA, {}).ok());
        QCOMPARE(h.f.state(id), state::interrupted);
        QSqlQuery audit(h.f.database);
        QVERIFY(audit.exec("SELECT source,user_client_id FROM managed_job_cleanup_audit "
                           "WHERE task_id='" +
                           id + "'"));
        QVERIFY(audit.next());
        QCOMPARE(audit.value(0).toString(), QStringLiteral("runner_removed"));
        QCOMPARE(audit.value(1).toString(), QStringLiteral("daemon"));
    }

    void aRemovedRunnerNamesTheLocalClientThatAsked() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        QVERIFY(h.write(syncingSteps(id, QStringLiteral("job-1"), runnerA, std::nullopt)));
        h.build();
        h.jobs->restore();
        QVERIFY(h.jobs->runnerRemoved(runnerA, QStringLiteral("connection-7")).ok());
        QCOMPARE(h.f.count("SELECT count(*) FROM managed_job_cleanup_audit WHERE "
                           "user_client_id='connection-7'"),
                 1);
    }

    void cleanupIsResolvedOnlyWithAUserAcknowledgement() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        QList<RemoteJobRecord> steps = syncingSteps(id, QStringLiteral("job-1"), runnerA, {});
        RemoteJobRecord attention = steps.last();
        attention.state = state::needsAttention;
        attention.reason = QStringLiteral("cleanup_unverified");
        attention.cleanupStatus = cleanup::unverified;
        steps << attention;
        QVERIFY(h.write(steps));
        h.build();
        h.jobs->restore();
        QCOMPARE(h.jobs->resolveCleanup(id, {}).code, QStringLiteral("invalid_request"));
        QCOMPARE(h.f.state(id), state::needsAttention);
        QVERIFY(h.jobs->resolveCleanup(id, QStringLiteral("connection-7")).ok());
        QCOMPARE(h.f.state(id), state::interrupted);
        QCOMPARE(h.jobs->resolveCleanup(id, QStringLiteral("connection-7")).code,
                 QStringLiteral("invalid_state"));
    }
};

QTEST_GUILESS_MAIN(RemoteJobsSubmitTest)
#include "RemoteJobsSubmitTest.moc"
