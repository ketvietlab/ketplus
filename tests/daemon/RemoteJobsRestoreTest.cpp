// A daemon restart through RemoteJobs: records the runner never took end interrupted and are
// cancelled on the Relay, an accepted record stays, an unreadable one ends once, and a local
// task is left alone.
#include "RemoteJobsHarness.h"

#include <QTest>

using namespace remote_jobs_test;
using remote_job_test::relayJob;
using remote_job_test::runnerA;

namespace {

int outboxOf(LedgerFixture& f, const QString& taskId) {
    return f.count("SELECT count(*) FROM managed_job_outbox WHERE task_id='" + taskId + "'");
}

bool cancelled(const remote_job_test::FakeRelay& relay, const QString& jobId) {
    for (const auto& call : relay.log)
        if (call.operation == QStringLiteral("cancel") && call.id == jobId &&
            call.reason == QStringLiteral("user"))
            return true;
    return false;
}

RemoteSnapshot aSnapshot() {
    return RemoteSnapshot{QStringLiteral("/repo/.git"),
                          QStringLiteral("/repo"),
                          QStringLiteral("0123456789abcdef0123456789abcdef01234567"),
                          {}};
}

} // namespace

class RemoteJobsRestoreTest final : public QObject {
    Q_OBJECT

  private slots:
    void recordsTheRunnerNeverTookEndInterruptedAndAreCancelled() {
        RemoteHarness h;
        const QString submitting = h.f.remoteTask();
        const QString queued = h.f.remoteTask();
        QVERIFY(h.write({recordOf(submitting)}));
        QVERIFY(h.write(queuedSteps(queued, QStringLiteral("job-q"))));
        const QString held = h.f.remoteTask();
        QList<RemoteJobRecord> steps = syncingSteps(held, QStringLiteral("job-h"), runnerA, {});
        steps.removeLast(); // dispatched, never accepted
        QVERIFY(h.write(steps));

        h.build();
        h.jobs->restore();
        for (const QString& id : {submitting, queued, held}) {
            QCOMPARE(h.f.state(id), state::interrupted);
            QCOMPARE(h.f.task(id).value("reason").toString(), QStringLiteral("daemon_restarted"));
            QCOMPARE(h.f.task(id).value("cleanup_status").toString(), cleanup::notRequired);
            QCOMPARE(outboxOf(h.f, id), 1);
        }
        QVERIFY(cancelled(h.relay, QStringLiteral("job-q")));
        QVERIFY(cancelled(h.relay, QStringLiteral("job-h")));
        // The record without a job id learns it under the same key, then cancels it.
        QCOMPARE(h.relay.pending(QStringLiteral("submit")), 1);
        QCOMPARE(h.relay.calls.first().id, submitting);
        RelayJob named = relayJob(QStringLiteral("queued"));
        named.jobId = QStringLiteral("job-s");
        QVERIFY(h.relay.answer(QStringLiteral("submit"), named));
        QVERIFY(cancelled(h.relay, QStringLiteral("job-s")));
        QVERIFY(!h.jobs->record(submitting) || h.jobs->record(submitting)->terminal());
    }

    void aRelayCancelIsRepeatedUntilAnswered() {
        RemoteHarness h;
        const QString queued = h.f.remoteTask();
        QVERIFY(h.write(queuedSteps(queued, QStringLiteral("job-q"))));
        h.build();
        h.jobs->restore();
        QVERIFY(h.relay.refuse(QStringLiteral("cancel"), QStringLiteral("network")));
        QCOMPARE(h.relay.sent(QStringLiteral("cancel")), 1);
        QCOMPARE(h.run(1000), 1);
        QCOMPARE(h.relay.sent(QStringLiteral("cancel")), 2);
        QVERIFY(h.relay.refuse(QStringLiteral("cancel"), QStringLiteral("job_not_found")));
        QCOMPARE(h.run(60'000), 0);
        QCOMPARE(h.relay.sent(QStringLiteral("cancel")), 2);
    }

    void anAcceptedRecordStays() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        QVERIFY(h.write(syncingSteps(id, QStringLiteral("job-1"), runnerA, aSnapshot())));
        h.build();
        h.jobs->restore();
        QCOMPARE(h.f.state(id), state::starting);
        const auto kept = h.jobs->record(id);
        QVERIFY(kept);
        QCOMPARE(kept->placement, placement::syncing);
        QCOMPARE(kept->snapshot, std::optional<RemoteSnapshot>(aSnapshot()));
        QCOMPARE(h.relay.sent(QStringLiteral("cancel")), 0);
        QCOMPARE(outboxOf(h.f, id), 0);
    }

    void anUnreadableRecordEndsOnceAcrossRestarts() {
        RemoteHarness h;
        const QString id = h.f.remoteTask();
        QVERIFY(h.write(queuedSteps(id, QStringLiteral("job-u"))));
        QVERIFY(h.f.exec("UPDATE remote_jobs SET record='{not json' WHERE task_id='" + id + "'"));
        h.build();
        h.jobs->restore();
        QCOMPARE(h.f.state(id), state::interrupted);
        QCOMPARE(h.f.task(id).value("reason").toString(), QStringLiteral("record_unreadable"));
        QCOMPARE(h.f.task(id).value("cleanup_status").toString(), cleanup::unverified);
        QCOMPARE(outboxOf(h.f, id), 1);
        QVERIFY(cancelled(h.relay, QStringLiteral("job-u")));

        h.build();
        h.jobs->restore();
        QCOMPARE(outboxOf(h.f, id), 1);
        QCOMPARE(h.relay.sent(QStringLiteral("cancel")), 1);
    }

    void aLocalTaskIsLeftAlone() {
        RemoteHarness h;
        const QString local =
            taskIdOf(h.f.ledger->create(intentFor(QStringLiteral("local"), std::nullopt)));
        QVERIFY(!local.isEmpty());
        const QJsonObject before = h.f.task(local);
        const QString remote = h.f.remoteTask();
        QVERIFY(h.write({recordOf(remote)}));
        h.build();
        h.jobs->restore();
        QCOMPARE(h.f.task(local), before);
        QCOMPARE(h.f.state(remote), state::interrupted);
    }

    void noJobIsTakenWhileTheRelayOffersNoRunners() {
        RemoteHarness h;
        h.relay.offered = false;
        QVERIFY(!h.jobs->available());
        QString code;
        h.jobs->submit(h.f.remoteTask(), SubmitRequest{{}, linuxLabels, 60'000},
                       [&](const QString& answer) { code = answer; });
        QCOMPARE(code, QStringLiteral("runner_unavailable"));
        QVERIFY(h.prepared.isEmpty());
        QVERIFY(h.relay.log.isEmpty());
    }
};

QTEST_GUILESS_MAIN(RemoteJobsRestoreTest)
#include "RemoteJobsRestoreTest.moc"
