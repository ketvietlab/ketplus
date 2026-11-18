#include "RemoteJobTestFakes.h"

#include <QtTest>

using namespace remote_job_test;

namespace {

constexpr qint64 minute = 60 * 1000;

void compareState(const RemoteJobRecord& record, const QString& expectedState,
                  const QString& expectedPlacement) {
    QCOMPARE(record.state, expectedState);
    QCOMPARE(record.placement, expectedPlacement);
}

} // namespace

class RemoteJobRecordTest final : public QObject {
    Q_OBJECT

  private slots:
    // ---- Placement table ----

    void placementFollowsTheTable() {
        Harness h;
        QVERIFY(h.start());
        compareState(h.record(), state::queued, placement::submitting);
        QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        QCOMPARE(h.relay.log.first().id, task);
        QCOMPARE(h.relay.log.first().runsOn, QStringList{"os:linux"});

        QVERIFY(h.relay.answer("submit", relayJob("queued")));
        compareState(h.record(), state::queued, placement::relayQueued);
        QCOMPARE(h.record().jobId, job);

        h.jobs->jobChanged(job);
        QVERIFY(h.relay.answer("get", relayJob("acquired", runnerA, 1)));
        compareState(h.record(), state::starting, placement::assigned);
        QCOMPARE(h.record().runner, (RemoteRunner{runnerA, "build-a", 1}));

        QVERIFY(h.jobs->offer(job, runnerA, 1).ok());
        QVERIFY(h.dispatch(runnerA).ok());
        compareState(h.record(), state::starting, placement::assigned);
        h.jobs->accepted(job, runnerA);
        compareState(h.record(), state::starting, placement::syncing);
        h.jobs->started(job, runnerA);
        compareState(h.record(), state::running, placement::running);
        QCOMPARE(h.record().runner, (RemoteRunner{runnerA, "build-a", 1}));

        h.jobs->result(job, runnerA, jobResult("exited", 0));
        compareState(h.record(), state::completed, placement::running);
        QCOMPARE(h.record().cleanupStatus, cleanup::verified);
        QCOMPARE(h.specs, 1);
    }

    void verifiedOfferAssignsBeforeTheRelaySaysSo() {
        Harness h;
        QVERIFY(h.queued());
        QVERIFY(h.jobs->offer(job, runnerB, 2).ok());
        compareState(h.record(), state::starting, placement::assigned);
        QCOMPARE(h.record().runner, (RemoteRunner{runnerB, "build-b", 2}));
    }

    void offerBeforeTheJobIsKnownIsAMismatch() {
        Harness h;
        QVERIFY(h.start());
        QCOMPARE(h.jobs->offer(job, runnerA, 1).code, QStringLiteral("job_mismatch"));
        QCOMPARE(h.jobs->offer("job-other", runnerA, 1).code, QStringLiteral("job_mismatch"));
    }

    void requeueBeforeDispatchDropsTheRunner() {
        Harness h;
        QVERIFY(h.assigned());
        h.jobs->jobChanged(job);
        QVERIFY(h.relay.answer("get", relayJob("queued", std::nullopt, 1)));
        compareState(h.record(), state::starting, placement::relayQueued);
        QVERIFY(!h.record().runner);
        h.jobs->jobChanged(job);
        QVERIFY(h.relay.answer("get", relayJob("acquired", runnerB, 2)));
        compareState(h.record(), state::starting, placement::assigned);
        QCOMPARE(h.record().runner->attempt, 2);
    }

    void eventsOutOfOrderNeverGoBack() {
        Harness h;
        QVERIFY(h.running());
        const RemoteJobRecord before = h.record();
        h.jobs->accepted(job, runnerA);
        h.jobs->started(job, runnerA);
        QCOMPARE(h.jobs->offer(job, runnerA, 1).code, QString()); // resume, nothing changes
        h.jobs->jobChanged(job);                                  // dispatched: no Relay read
        QCOMPARE(h.relay.pending("get"), 0);
        QCOMPARE(h.record(), before);

        Harness early;
        QVERIFY(early.queued());
        early.jobs->accepted(job, runnerA); // nothing was dispatched
        early.jobs->started(job, runnerA);
        early.jobs->result(job, runnerA, jobResult("exited", 0));
        compareState(early.record(), state::queued, placement::relayQueued);
        early.jobs->jobChanged(job);
        QVERIFY(early.relay.answer("get", relayJob("acquired", runnerA, 1)));
        early.jobs->jobChanged(job);
        QVERIFY(early.relay.answer("get", relayJob("offered")));
        early.jobs->jobChanged(job);
        QVERIFY(early.relay.answer("get", relayJob("started", runnerA, 1)));
        compareState(early.record(), state::starting, placement::relayQueued);
    }

    void terminalRecordsNeverChange() {
        Harness h;
        QVERIFY(h.running());
        h.jobs->result(job, runnerA, jobResult("exited", 3));
        const RemoteJobRecord ended = h.record();
        QCOMPARE(ended.state, state::failed);
        const int writes = h.store.writes;
        h.jobs->result(job, runnerA, jobResult("exited", 0));
        h.jobs->result(job, runnerA, jobResult("cancelled"));
        h.jobs->accepted(job, runnerA);
        h.jobs->started(job, runnerA);
        h.jobs->rejected(job, runnerA, "spec_invalid");
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.jobs->jobChanged(job);
        h.jobs->runnerRemoved(runnerA);
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.jobs->resolveCleanup(task).code, QStringLiteral("invalid_state"));
        QCOMPARE(h.jobs->offer(job, runnerA, 1).code, QStringLiteral("job_mismatch"));
        h.clock.advance(60 * minute);
        QCOMPARE(h.record(), ended);
        QCOMPARE(h.store.writes, writes);
    }

    // ---- Terminal mapping ----

    void resultsMapToTheirTerminalState_data() {
        QTest::addColumn<QString>("resultState");
        QTest::addColumn<qint64>("exitCode");
        QTest::addColumn<qint64>("signal");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<QString>("expectedState");
        QTest::addColumn<QString>("expectedReason");
        QTest::newRow("exit 0") << "exited" << 0LL << -1LL << "" << "completed" << "";
        QTest::newRow("exit 1") << "exited" << 1LL << -1LL << "" << "failed" << "exit_code";
        QTest::newRow("signal") << "exited" << -1LL << 9LL << "" << "failed" << "exit_code";
        QTest::newRow("exit 0 with signal") << "exited" << 0LL << 15LL << "" << "failed"
                                            << "exit_code";
        for (const char* code : {"snapshot_failed", "snapshot_too_large", "pack_invalid",
                                 "disk_full", "checkout_failed"})
            QTest::addRow("failed %s", code)
                << "failed" << -1LL << -1LL << code << "failed" << "snapshot_failed";
        QTest::newRow("failed spawn") << "failed" << -1LL << -1LL << "spawn_failed" << "failed"
                                      << "spawn_failed";
        QTest::newRow("cancelled") << "cancelled" << -1LL << 15LL << "user" << "cancelled"
                                   << "user";
        QTest::newRow("timed out") << "timed_out" << -1LL << 9LL << "deadline" << "timed_out"
                                   << "deadline";
        for (const char* reason :
             {"runner_restarted", "lease_lost", "requester_lost", "entitlement_required"})
            QTest::addRow("interrupted %s", reason)
                << "interrupted" << -1LL << -1LL << reason << "interrupted" << reason;
    }
    void resultsMapToTheirTerminalState() {
        QFETCH(QString, resultState);
        QFETCH(qint64, exitCode);
        QFETCH(qint64, signal);
        QFETCH(QString, reason);
        QFETCH(QString, expectedState);
        QFETCH(QString, expectedReason);
        JobResult value{resultState, std::nullopt, std::nullopt, reason, "verified"};
        if (exitCode >= 0)
            value.exitCode = exitCode;
        if (signal >= 0)
            value.signal = signal;
        for (const bool stopped : {false, true}) {
            Harness h;
            QVERIFY(h.running());
            if (stopped)
                QVERIFY(h.jobs->stop(task).ok());
            h.jobs->result(job, runnerA, value);
            QCOMPARE(h.record().state, expectedState);
            QCOMPARE(h.record().reason, expectedReason);
            QCOMPARE(h.record().cleanupStatus, cleanup::verified);
            QCOMPARE(h.record().exitCode, value.exitCode);

            // The same result without verified cleanup needs the user's attention.
            Harness u;
            QVERIFY(u.running());
            if (stopped)
                QVERIFY(u.jobs->stop(task).ok());
            JobResult unverified = value;
            unverified.cleanupStatus = "unverified";
            u.jobs->result(job, runnerA, unverified);
            QCOMPARE(u.record().state, state::needsAttention);
            QCOMPARE(u.record().reason, QStringLiteral("cleanup_unverified"));
            QCOMPARE(u.record().cleanupStatus, cleanup::unverified);
            QCOMPARE(u.record().exitCode, value.exitCode);
        }
    }

    void relayEndsAreMappedBeforeDispatch_data() {
        QTest::addColumn<QString>("relayState");
        QTest::addColumn<QString>("relayReason");
        QTest::addColumn<QString>("expectedReason");
        QTest::newRow("expired") << "expired" << "" << "queue_expired";
        for (const char* reason :
             {"delivery_exhausted", "not_paired", "runner_removed", "entitlement_required",
              "runner_unavailable", "no_matching_runner"})
            QTest::addRow("failed %s", reason) << "failed" << reason << reason;
        QTest::newRow("failed other") << "failed" << "something_new" << "runner_unavailable";
        QTest::newRow("failed without reason") << "failed" << "" << "runner_unavailable";
        QTest::newRow("cancelled") << "cancelled" << "" << "runner_unavailable";
        QTest::newRow("finished") << "finished" << "" << "runner_unavailable";
        QTest::newRow("abandoned") << "abandoned" << "" << "runner_unavailable";
    }
    void relayEndsAreMappedBeforeDispatch() {
        QFETCH(QString, relayState);
        QFETCH(QString, relayReason);
        QFETCH(QString, expectedReason);
        const std::optional<QString> reason =
            relayReason.isEmpty() ? std::nullopt : std::optional(relayReason);
        for (const QString& stage : {"queued", "assigned"}) {
            Harness h;
            QVERIFY(stage == "queued" ? h.queued() : h.assigned());
            h.jobs->jobChanged(job);
            QVERIFY(h.relay.answer("get", relayJob(relayState, runnerA, 1, reason)));
            QCOMPARE(h.record().state, state::failed);
            QCOMPARE(h.record().reason, expectedReason);
            QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        }
        // Once the spec may have reached a runner, the Relay never changes the record.
        RemoteJobRecord sent;
        sent.taskId = task;
        sent.jobId = job;
        sent.state = state::starting;
        sent.placement = placement::assigned;
        sent.dispatchedTo = runnerA;
        QVERIFY(!applyRelayJob(sent, relayJob(relayState, runnerA, 1, reason), "build-a"));
        QVERIFY(!applyRelayJob(sent, relayJob("queued"), {}));
        Harness d;
        QVERIFY(d.running());
        d.jobs->jobChanged(job);
        QCOMPARE(d.relay.pending("get"), 0);
        QCOMPARE(d.record().state, state::running);
    }

    void peerErrorsBeforeLaunchEndTheRecord_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expectedReason");
        QTest::newRow("spec invalid") << "spec_invalid" << "spec_rejected";
        QTest::newRow("containment") << "containment_unavailable" << "containment_unavailable";
        QTest::newRow("withdrawn") << "runner_unavailable" << "runner_unavailable";
    }
    void peerErrorsBeforeLaunchEndTheRecord() {
        QFETCH(QString, code);
        QFETCH(QString, expectedReason);
        Harness h;
        QVERIFY(h.sent());
        h.jobs->rejected(job, runnerA, code);
        QCOMPARE(h.record().state, state::failed);
        QCOMPARE(h.record().reason, expectedReason);
        QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        QVERIFY(!h.record().dispatched());
    }

    void rejectionsThatLeaveTheJobOpenClearTheMark_data() {
        QTest::addColumn<QString>("code");
        for (const char* code : {"lease_lost", "not_paired", "job_mismatch", "label_mismatch",
                                 "runner_busy", "unsupported_frame"})
            QTest::newRow(code) << QString::fromLatin1(code);
    }
    void rejectionsThatLeaveTheJobOpenClearTheMark() {
        QFETCH(QString, code);
        Harness h;
        QVERIFY(h.sent());
        h.jobs->rejected(job, runnerA, code);
        QVERIFY(!h.record().dispatched());
        QCOMPARE(h.record().state, state::starting);
        // The Relay is read again and the job goes to the next runner.
        QVERIFY(h.relay.answer("get", relayJob("acquired", runnerB, 2)));
        QVERIFY(h.jobs->offer(job, runnerB, 2).ok());
        QVERIFY(h.dispatch(runnerB).ok());
        QCOMPARE(h.record().dispatchedTo, runnerB);
    }

    void cancelOfTheFirstRunnerEndsAfterLeaseLost() {
        Harness h;
        QVERIFY(h.sent());
        QCOMPARE(h.jobs->offer(job, runnerB, 2).code, QStringLiteral("already_dispatched"));
        h.jobs->rejected(job, runnerA, "lease_lost");
        QVERIFY(h.relay.answer("get", relayJob("cancelled", std::nullopt, 1, "user")));
        QCOMPARE(h.record().state, state::failed);
        QCOMPARE(h.record().reason, QStringLiteral("runner_unavailable"));
    }

    void deadlineMovesARunningRecordToStopping() {
        Harness h;
        QVERIFY(h.running(5000));
        h.clock.advance(4999);
        QCOMPARE(h.record().state, state::running);
        h.clock.advance(1);
        QCOMPARE(h.record().state, state::stopping);
        QCOMPARE(h.record().stopReason, QStringLiteral("deadline"));
        QCOMPARE(h.record().cleanupStatus, cleanup::pending);
        QCOMPARE(h.relay.sent("cancel"), 0);
        h.jobs->result(job, runnerA, jobResult("timed_out", std::nullopt, "deadline"));
        QCOMPARE(h.record().state, state::timedOut);

        // No deadline before job.started: waiting is bounded by the Relay only.
        Harness w;
        QVERIFY(w.syncing(5000));
        w.clock.advance(10 * minute);
        QCOMPARE(w.record().state, state::starting);
    }

    void aDeadlineAfterAStopOrAttentionChangesNothing() {
        Harness h;
        QVERIFY(h.running(5000));
        QVERIFY(h.jobs->stop(task).ok());
        const RemoteJobRecord stopped = h.record();
        h.clock.advance(5000);
        QCOMPARE(h.record(), stopped);
        QCOMPARE(h.record().stopReason, QStringLiteral("user"));

        Harness a;
        QVERIFY(a.running(30 * minute));
        a.jobs->peerLost(job, runnerA, a.clock.time);
        a.clock.advance(20 * minute);
        const RemoteJobRecord attention = a.record();
        QCOMPARE(attention.state, state::needsAttention);
        a.jobs->timerFired(task, JobTimer::Deadline);
        QCOMPARE(a.record(), attention);
    }

    void aResultAfterTheDeadlineDecides() {
        Harness h;
        QVERIFY(h.running(1000));
        h.clock.advance(1000);
        h.jobs->result(job, runnerA, jobResult("exited", 0));
        QCOMPARE(h.record().state, state::completed);
    }
};

QTEST_GUILESS_MAIN(RemoteJobRecordTest)
#include "RemoteJobRecordTest.moc"
