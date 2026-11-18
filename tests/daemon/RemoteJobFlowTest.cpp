#include "RemoteJobTestFakes.h"

#include <QtTest>

using namespace remote_job_test;

namespace {

constexpr qint64 minute = 60 * 1000;

} // namespace

class RemoteJobFlowTest final : public QObject {
    Q_OBJECT

  private slots:
    // ---- Cancel ----

    void stopWhileSubmittingCancelsOnceTheJobIsKnown() {
        Harness h;
        QVERIFY(h.start());
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.record().state, state::stopping);
        QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        QCOMPARE(h.relay.pending("submit"), 2); // the first one and one to learn the job
        QVERIFY(h.relay.answer("submit", relayJob("queued")));
        QCOMPARE(h.relay.log.last().operation, QStringLiteral("cancel"));
        QCOMPARE(h.relay.log.last().reason, QStringLiteral("user"));
        QCOMPARE(h.record().state, state::stopping);
        QVERIFY(h.relay.answer("cancel", relayJob("cancelled")));
        QCOMPARE(h.record().state, state::cancelled);
        QCOMPARE(h.record().reason, QStringLiteral("user"));
        QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        QVERIFY(h.relay.answer("submit", relayJob("cancelled"))); // a late answer changes nothing
        QCOMPARE(h.record().state, state::cancelled);
    }

    void stopWhileSubmittingEndsCancelledWhenTheSubmitIsRefused() {
        Harness h;
        QVERIFY(h.start());
        QVERIFY(h.jobs->stop(task).ok());
        QVERIFY(h.relay.refuse("submit", "no_matching_runner"));
        QCOMPARE(h.record().state, state::cancelled);
        QCOMPARE(h.record().reason, QStringLiteral("user"));
    }

    void stopWhileRelayQueuedCancelsOnTheRelay() {
        Harness h;
        QVERIFY(h.queued());
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.relay.pending("cancel"), 1);
        QVERIFY(h.peers.cancels.isEmpty());
        QVERIFY(h.relay.refuse("cancel", "network"));
        QCOMPARE(h.record().state, state::stopping);
        h.clock.advance(1000);
        QVERIFY(h.relay.answer("cancel", relayJob("cancelled")));
        QCOMPARE(h.record().state, state::cancelled);
        QCOMPARE(h.relay.sent("cancel"), 2);
    }

    void stopWhileAssignedCancelsOnThePeerAndTheRelay() {
        Harness h;
        QVERIFY(h.assigned());
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.peers.cancels, (QList<std::pair<QString, QString>>{{task, runnerA}}));
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("job_mismatch"));
        QVERIFY(h.relay.answer("cancel", relayJob("cancelled")));
        QCOMPARE(h.record().state, state::cancelled);
        QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        QCOMPARE(h.specs, 0);
    }

    void stopAfterTheSpecWaitsForAVerifiedResult_data() {
        QTest::addColumn<QString>("stage");
        QTest::newRow("assigned, spec sent") << "sent";
        QTest::newRow("syncing") << "syncing";
        QTest::newRow("running") << "running";
    }
    void stopAfterTheSpecWaitsForAVerifiedResult() {
        QFETCH(QString, stage);
        Harness h;
        QVERIFY(stage == "sent" ? h.sent() : stage == "syncing" ? h.syncing() : h.running());
        const QString placementBefore = h.record().placement;
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.record().state, state::stopping);
        QCOMPARE(h.record().placement, placementBefore);
        QCOMPARE(h.record().cleanupStatus, cleanup::pending);
        QCOMPARE(h.peers.cancels.size(), 1);
        QCOMPARE(h.relay.pending("cancel"), 1);
        QVERIFY(h.relay.answer("cancel", relayJob("cancelled")));
        QCOMPARE(h.record().state, state::stopping); // the Relay's answer proves nothing
        h.jobs->result(job, runnerA, jobResult("cancelled", std::nullopt, "user"));
        QCOMPARE(h.record().state, state::cancelled);
        QCOMPARE(h.record().cleanupStatus, cleanup::verified);
    }

    void stopIsIdempotent() {
        Harness h;
        QVERIFY(h.running());
        QVERIFY(h.jobs->stop(task).ok());
        const RemoteJobRecord stopped = h.record();
        const int calls = h.relay.log.size();
        QVERIFY(h.jobs->stop(task).ok());
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.record(), stopped);
        QCOMPARE(h.relay.log.size(), calls);
        QCOMPARE(h.peers.cancels.size(), 1);
    }

    void resultAndStopCommitOneTerminalState() {
        Harness first;
        QVERIFY(first.running());
        first.jobs->result(job, runnerA, jobResult("exited", 0));
        QVERIFY(first.jobs->stop(task).ok());
        QCOMPARE(first.record().state, state::completed);

        Harness second;
        QVERIFY(second.running());
        QVERIFY(second.jobs->stop(task).ok());
        second.jobs->result(job, runnerA, jobResult("exited", 0));
        second.jobs->result(job, runnerA, jobResult("cancelled", std::nullopt, "user"));
        QCOMPARE(second.record().state, state::completed);
    }

    // ---- Peer loss and needs_attention ----

    void peerLossKeepsTheStateForTwentyMinutes_data() {
        QTest::addColumn<QString>("stage");
        QTest::addColumn<bool>("stopped");
        QTest::newRow("syncing") << "syncing" << false;
        QTest::newRow("running") << "running" << false;
        QTest::newRow("stopping") << "running" << true;
    }
    void peerLossKeepsTheStateForTwentyMinutes() {
        QFETCH(QString, stage);
        QFETCH(bool, stopped);
        Harness h;
        QVERIFY(stage == "syncing" ? h.syncing() : h.running());
        if (stopped)
            QVERIFY(h.jobs->stop(task).ok());
        const QString kept = h.record().state;
        const QDateTime lastContact = h.clock.time.addSecs(-60);
        h.jobs->peerLost(job, runnerA, lastContact);
        QCOMPARE(h.record().state, kept);
        QCOMPARE(h.record().lastPeerContact, std::optional(lastContact));
        h.clock.advance(19 * minute - 1); // one step before 20 minutes after the last contact
        QCOMPARE(h.record().state, kept);
        h.clock.advance(1);
        QCOMPARE(h.record().state, state::needsAttention);
        QCOMPARE(h.record().reason, QStringLiteral("cleanup_unverified"));
        QCOMPARE(h.record().cleanupStatus, cleanup::unverified);
    }

    void aPeerThatReturnsStopsTheWindow() {
        Harness h;
        QVERIFY(h.running());
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(10 * minute);
        h.jobs->peerOpened(job, runnerA);
        h.clock.advance(30 * minute);
        QCOMPARE(h.record().state, state::running);
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        QCOMPARE(h.record().state, state::needsAttention);
    }

    void relayEndWithoutAResultWaitsForTheWindow_data() {
        QTest::addColumn<QString>("relayState");
        QTest::newRow("finished") << "finished";
        QTest::newRow("abandoned") << "abandoned";
        QTest::newRow("cancelled") << "cancelled";
        QTest::newRow("failed") << "failed";
    }
    void relayEndWithoutAResultWaitsForTheWindow() {
        QFETCH(QString, relayState);
        Harness h;
        QVERIFY(h.running());
        h.jobs->jobChanged(job);
        QCOMPARE(h.relay.pending("get"), 0);
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        QCOMPARE(h.record().state, state::needsAttention);
    }

    void assignedDispatchedRecordUsesTheWindowToo() {
        Harness h;
        QVERIFY(h.sent());
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        QCOMPARE(h.record().state, state::needsAttention);
    }

    void attentionLeavesByTheUsersAcknowledgement() {
        Harness h;
        QVERIFY(h.running());
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        QCOMPARE(h.jobs->resolveCleanup(task).code, QString());
        QCOMPARE(h.record().state, state::interrupted);
        QCOMPARE(h.record().reason, QStringLiteral("cleanup_acknowledged"));
        QCOMPARE(h.record().cleanupStatus, cleanup::unverified);
    }

    void attentionLeavesByAVerifiedResultOfThePinnedRunner() {
        Harness h;
        QVERIFY(h.running());
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        h.jobs->result(job, runnerB, jobResult("exited", 0)); // not the runner holding it
        h.jobs->result(job, runnerA, jobResult("exited", 0, {}, "unverified"));
        QCOMPARE(h.record().state, state::needsAttention);
        h.jobs->peerOpened(job, runnerA);
        h.jobs->result(job, runnerA,
                       jobResult("interrupted", std::nullopt, "requester_lost", "verified"));
        QCOMPARE(h.record().state, state::interrupted);
        QCOMPARE(h.record().reason, QStringLiteral("requester_lost"));
        QCOMPARE(h.record().cleanupStatus, cleanup::verified);
    }

    void attentionHasNoOtherWayOut() {
        Harness h;
        QVERIFY(h.running());
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        const RemoteJobRecord attention = h.record();
        QVERIFY(h.jobs->stop(task).ok());
        h.jobs->jobChanged(job);
        h.jobs->accepted(job, runnerA);
        h.jobs->started(job, runnerA);
        h.jobs->rejected(job, runnerA, "spec_invalid");
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(120 * minute);
        h.jobs->runnerRemoved("runner-other");
        h.trust.pins.remove(runnerA); // a verified result from an unpinned runner is not trusted
        h.jobs->result(job, runnerA, jobResult("exited", 0));
        QCOMPARE(h.record(), attention);
    }

    void removingTheRunnerEndsItsRecordsAtOnce_data() {
        QTest::addColumn<QString>("stage");
        QTest::newRow("assigned, spec sent") << "sent";
        QTest::newRow("syncing") << "syncing";
        QTest::newRow("running") << "running";
        QTest::newRow("stopping") << "stopping";
        QTest::newRow("needs attention") << "attention";
    }
    void removingTheRunnerEndsItsRecordsAtOnce() {
        QFETCH(QString, stage);
        Harness h;
        QVERIFY(stage == "sent" ? h.sent() : stage == "syncing" ? h.syncing() : h.running());
        if (stage == "stopping")
            QVERIFY(h.jobs->stop(task).ok());
        if (stage == "attention") {
            h.jobs->peerLost(job, runnerA, h.clock.time);
            h.clock.advance(20 * minute);
        }
        QList<QString> states;
        h.jobs->setChanged([&](const RemoteJobRecord& r) { states.append(r.state); });
        const int cancels = h.relay.sent("cancel");
        h.trust.pins.remove(runnerA);
        h.jobs->runnerRemoved(runnerA);
        QCOMPARE(h.record().state, state::interrupted);
        QCOMPARE(h.record().reason, QStringLiteral("cleanup_acknowledged"));
        QCOMPARE(states.last(), state::interrupted);
        QCOMPARE(states.contains(state::needsAttention) || stage == "attention", true);
        QCOMPARE(h.relay.sent("cancel"), cancels + 1);
        QCOMPARE(h.relay.log.last().reason, QStringLiteral("runner_removed"));
    }

    void removingARunnerBeforeDispatchReadsTheRelay() {
        Harness h;
        QVERIFY(h.assigned());
        h.jobs->runnerRemoved(runnerA);
        QVERIFY(h.relay.answer("get", relayJob("failed", runnerA, 1, "runner_removed")));
        QCOMPARE(h.record().state, state::failed);
        QCOMPARE(h.record().reason, QStringLiteral("runner_removed"));
    }

    // ---- At-most-once ----

    void theMarkIsDurableBeforeTheSpec() {
        Harness h;
        QVERIFY(h.assigned());
        h.store.failWrites = true;
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("storage_error"));
        QVERIFY(!h.record().dispatched());
        h.store.failWrites = false;
        QVERIFY(h.dispatch(runnerA).ok());
        QCOMPARE(h.store.find(task)->dispatchedTo, runnerA);
        QCOMPARE(h.specs, 1);
    }

    void aSecondRunnerIsRefused() {
        Harness h;
        QVERIFY(h.sent());
        QCOMPARE(h.jobs->offer(job, runnerB, 2).code, QStringLiteral("already_dispatched"));
        QCOMPARE(h.relay.log.last().operation, QStringLiteral("cancel"));
        QCOMPARE(h.relay.log.last().reason, QStringLiteral("already_dispatched"));
        QCOMPARE(h.dispatch(runnerB).code, QStringLiteral("already_dispatched"));
        h.jobs->accepted(job, runnerB);
        h.jobs->started(job, runnerB);
        h.jobs->result(job, runnerB, jobResult("exited", 0));
        QCOMPARE(h.record().dispatchedTo, runnerA);
        QCOMPARE(h.record().placement, placement::assigned);
        QCOMPARE(h.record().runner->runnerId, runnerA);
        QCOMPARE(h.specs, 1);
    }

    void theSpecIsAllowedOnce() {
        Harness h;
        QVERIFY(h.running());
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("already_dispatched"));
        QVERIFY(h.jobs->offer(job, runnerA, 1).ok()); // the same runner resumes
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("already_dispatched"));
        h.reopen();
        h.jobs->restore();
        QVERIFY(h.jobs->offer(job, runnerA, 1).ok());
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("already_dispatched"));
        QCOMPARE(h.specs, 1);
    }

    void unpinnedRunnersNeverGetASpec() {
        Harness h;
        QVERIFY(h.queued());
        QCOMPARE(h.jobs->offer(job, "runner-x", 1).code, QStringLiteral("not_paired"));
        QVERIFY(h.jobs->offer(job, runnerA, 1).ok());
        h.trust.pins.remove(runnerA);
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("not_paired"));
        QCOMPARE(h.dispatch("runner-x").code, QStringLiteral("not_paired"));
        QCOMPARE(h.specs, 0);
    }

    void aStoppedJobGetsNoSpec() {
        Harness h;
        QVERIFY(h.assigned());
        QVERIFY(h.jobs->stop(task).ok());
        QCOMPARE(h.dispatch(runnerA).code, QStringLiteral("job_mismatch"));
        QCOMPARE(h.jobs->offer(job, runnerB, 2).code, QStringLiteral("job_mismatch"));
        QCOMPARE(h.specs, 0);
    }

    // ---- Submit and recovery ----

    void lostSubmitAnswersAreRetriedWithBackoff() {
        Harness h;
        QVERIFY(h.start());
        QList<qint64> delays;
        for (int round = 0; round < 7; ++round) {
            QVERIFY(h.relay.refuse("submit", round % 2 ? "relay_error" : "network"));
            delays.append(h.clock.armed.last().second);
            h.clock.advance(delays.last());
        }
        QCOMPARE(delays, (QList<qint64>{1000, 2000, 4000, 8000, 16000, 30000, 30000}));
        QCOMPARE(h.relay.sent("submit"), 8);
        for (const auto& call : h.relay.log)
            QCOMPARE(call.id, task);
        QVERIFY(h.relay.answer("submit", relayJob("queued")));
        QCOMPARE(h.record().placement, placement::relayQueued);
    }

    void submitRefusalsEndTheRecord_data() {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("reason");
        for (const char* code : {"no_matching_runner", "queue_full", "invalid_labels",
                                 "idempotency_conflict", "entitlement_required"})
            QTest::newRow(code) << code << code;
        QTest::newRow("unsupported") << "unsupported" << "runner_unavailable";
    }
    void submitRefusalsEndTheRecord() {
        QFETCH(QString, code);
        QFETCH(QString, reason);
        Harness h;
        QVERIFY(h.start());
        QVERIFY(h.relay.refuse("submit", code));
        QCOMPARE(h.record().state, state::failed);
        QCOMPARE(h.record().reason, reason);
        QCOMPARE(h.record().cleanupStatus, cleanup::notRequired);
        QVERIFY(!h.clock.isArmed(task, JobTimer::RelayRetry));
    }

    void anIdempotentSubmitAnswerIsMapped() {
        Harness h;
        QVERIFY(h.start());
        QVERIFY(h.relay.answer("submit", relayJob("acquired", runnerB, 1)));
        QCOMPARE(h.record().placement, placement::assigned);
        QCOMPARE(h.record().runner->runnerId, runnerB);
        Harness ended;
        QVERIFY(ended.start());
        QVERIFY(ended.relay.answer("submit", relayJob("expired")));
        QCOMPARE(ended.record().reason, QStringLiteral("queue_expired"));
    }

    void restartResumesEachPlacement() {
        Harness h;
        const auto make = [&](const QString& id, const QString& placementValue,
                              const QString& stateValue, const QString& dispatchedTo) {
            RemoteJobRecord stored;
            stored.taskId = id;
            stored.runsOn = {"os:linux"};
            stored.state = stateValue;
            stored.placement = placementValue;
            stored.jobId = placementValue == placement::submitting ? QString() : "job-" + id;
            stored.dispatchedTo = dispatchedTo;
            if (!dispatchedTo.isEmpty()) // contacts long before the restart
                stored.lastPeerContact = h.clock.time.addSecs(-3 * 3600);
            if (placementValue != placement::submitting && placementValue != placement::relayQueued)
                stored.runner = RemoteRunner{runnerA, "build-a", 1};
            QVERIFY(h.store.put(stored, 0));
        };
        make("submitting", placement::submitting, state::queued, {});
        make("queued", placement::relayQueued, state::queued, {});
        make("assigned", placement::assigned, state::starting, {});
        make("sent", placement::assigned, state::starting, runnerA);
        make("syncing", placement::syncing, state::starting, runnerA);
        make("running", placement::running, state::running, runnerA);
        h.reopen();
        h.relay.log.clear();
        h.jobs->restore();
        QCOMPARE(h.relay.sent("submit"), 1);
        QCOMPARE(h.relay.sent("get"), 1);
        for (const auto& call : h.relay.log)
            QCOMPARE(call.id, call.operation == "get" ? QStringLiteral("job-queued")
                                                      : QStringLiteral("submitting"));
        QCOMPARE(h.relay.sent("cancel"), 0);
        for (const QString id : {"sent", "syncing", "running"}) {
            QCOMPARE(h.jobs->dispatch("job-" + id, runnerA).code,
                     QStringLiteral("already_dispatched"));
        }
        // The window of dispatched records counts from the restart.
        h.clock.advance(20 * minute - 1);
        QCOMPARE(h.record("running").state, state::running);
        h.clock.advance(1);
        QCOMPARE(h.record("running").state, state::needsAttention);
        QCOMPARE(h.record("assigned").state, state::starting); // waits for the runner's offer
    }

    void restartRepeatsTheUsersCancel() {
        Harness h;
        QVERIFY(h.running());
        QVERIFY(h.jobs->stop(task).ok());
        h.reopen();
        h.relay.log.clear();
        h.jobs->restore();
        QCOMPARE(h.relay.sent("cancel"), 1);

        Harness deadline;
        QVERIFY(deadline.running(1000));
        deadline.clock.advance(1000);
        deadline.reopen();
        deadline.relay.log.clear();
        deadline.jobs->restore();
        QCOMPARE(deadline.relay.sent("cancel"), 0);
    }

    // ---- Limits and store failures ----

    void sixtyFourOutstandingJobsAtMost() {
        Harness h;
        for (int index = 0; index < maximumOutstanding; ++index)
            QVERIFY(h.jobs->start(QStringLiteral("task-%1").arg(index), {"gpu"}, 0).ok());
        QCOMPARE(h.jobs->start("task-extra", {"gpu"}, 0).code,
                 QStringLiteral("resource_exhausted"));
        QVERIFY(!h.jobs->record("task-extra"));
        // An attention record still counts; a terminal one frees its place.
        QVERIFY(h.relay.answer("submit", relayJob("queued")));
        QVERIFY(h.jobs->offer(job, runnerA, 1).ok());
        QVERIFY(h.dispatch(runnerA).ok());
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        QCOMPARE(h.record("task-0").state, state::needsAttention);
        QCOMPARE(h.jobs->start("task-extra", {"gpu"}, 0).code,
                 QStringLiteral("resource_exhausted"));
        QVERIFY(h.relay.refuse("submit", "queue_full"));
        QVERIFY(h.jobs->start("task-extra", {"gpu"}, 0).ok());
    }

    void startIsRefusedBeforeAnythingIsSent() {
        Harness h;
        h.store.failWrites = true;
        QCOMPARE(h.jobs->start(task, {"gpu"}, 0).code, QStringLiteral("storage_error"));
        QCOMPARE(h.relay.log.size(), 0);
        QVERIFY(!h.jobs->record(task));
        h.store.failWrites = false;
        h.relay.offered = false;
        QCOMPARE(h.jobs->start(task, {"gpu"}, 0).code, QStringLiteral("runner_unavailable"));
        h.relay.offered = true;
        QCOMPARE(h.jobs->start(task, {"GPU"}, 0).code, QStringLiteral("invalid_labels"));
        QCOMPARE(h.jobs->start("task/1", {"gpu"}, 0).code, QStringLiteral("invalid_request"));
        QCOMPARE(h.relay.log.size(), 0);
        QVERIFY(h.jobs->start(task, {"gpu"}, 0).ok());
        QVERIFY(h.jobs->start(task, {"gpu"}, 0).ok()); // the same start again
        QCOMPARE(h.jobs->start(task, {"cpu"}, 0).code, QStringLiteral("idempotency_conflict"));
        QCOMPARE(h.relay.sent("submit"), 1);
    }

    void storeFailuresKeepTheRecordAndAreReported() {
        Harness h;
        QVERIFY(h.running());
        h.store.failWrites = true;
        QCOMPARE(h.jobs->stop(task).code, QStringLiteral("storage_error"));
        QCOMPARE(h.record().state, state::running);
        QCOMPARE(h.peers.cancels.size(), 0);
        QCOMPARE(h.relay.pending("cancel"), 0);
        h.jobs->peerLost(job, runnerA, h.clock.time);
        h.clock.advance(20 * minute);
        QCOMPARE(h.record().state, state::running); // retried while the store refuses
        h.store.failWrites = false;
        h.clock.advance(1000);
        QCOMPARE(h.record().state, state::needsAttention);
        h.store.failWrites = true;
        QCOMPARE(h.jobs->resolveCleanup(task).code, QStringLiteral("storage_error"));
        QCOMPARE(h.record().state, state::needsAttention);
    }
};

QTEST_GUILESS_MAIN(RemoteJobFlowTest)
#include "RemoteJobFlowTest.moc"
