#include "RunnerDeliveryTest.h"

#include <QJsonDocument>
#include <QTest>

namespace runner_delivery_test {

using State = JobDelivery::State;
using Kind = LeaseCall::Kind;
using Step = JournalRecord::Step;

namespace {

JournalRecord record(const Step step) {
    JournalRecord record;
    record.step = step;
    if (step == Step::Acquiring) {
        record.acquireId = QStringLiteral("acq-7");
        return record;
    }
    record.lease = heldLease();
    if (step == Step::Started || step == Step::Closing)
        record.startedAt = startedAt;
    if (step == Step::Closing) {
        record.outcome = QStringLiteral("succeeded");
        record.reason = QStringLiteral("exited");
    }
    return record;
}

} // namespace

void RunnerDeliveryRestartTest::journalStepsComeBeforeTheirEffects() {
    Host host;
    host.start();
    host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
    QCOMPARE(host.journal.steps(),
             (QStringList{"acquiring", "leased", "spec_accepted", "started", "closing"}));
    // acquire after "acquiring", start after "spec_accepted", finish after "closing".
    QCOMPARE(host.callKinds(), (QStringList{"acquire", "start", "finish"}));
    QCOMPARE(host.relay.journalWritesAtSend, (QList<int>{1, 3, 5}));
    // The session sees the lease only once "leased" is on disk.
    QCOMPARE(host.journal.records.at(1).lease.leaseId, host.session.opened.first().leaseId);
    host.answerOk({});
    QCOMPARE(host.journal.clears, 1);
}

void RunnerDeliveryRestartTest::journalHoldsNoJobContent() {
    const QJsonObject json = journalToJson(record(Step::Closing));
    QCOMPARE(QStringList(json.keys()),
             (QStringList{"attempt", "close", "jobId", "leaseExpiresAt", "leaseId", "machineId",
                          "outcome", "reason", "runsOn", "startedAt", "step", "version",
                          "workspaceId"}));
    const QJsonObject acquiring = journalToJson(record(Step::Acquiring));
    QCOMPARE(QStringList(acquiring.keys()), (QStringList{"acquireId", "step", "version"}));
}

void RunnerDeliveryRestartTest::journalRoundTripsAndRefusesCorruption() {
    for (const Step step :
         {Step::Acquiring, Step::Leased, Step::SpecAccepted, Step::Started, Step::Closing}) {
        const JournalRecord original = record(step);
        const std::optional<JournalRecord> back = journalFromJson(journalToJson(original));
        QVERIFY(back);
        QCOMPARE(int(back->step), int(step));
        QCOMPARE(back->acquireId, original.acquireId);
        QCOMPARE(back->lease.leaseId, original.lease.leaseId);
        QCOMPARE(back->lease.runsOn, original.lease.runsOn);
        QCOMPARE(back->startedAt, original.startedAt);
        QCOMPARE(back->outcome, original.outcome);
    }
    const QJsonObject good = journalToJson(record(Step::Closing));
    const auto broken = [&](const QString& key, const QJsonValue& value) {
        QJsonObject copy = good;
        if (value.isUndefined())
            copy.remove(key);
        else
            copy.insert(key, value);
        return !journalFromJson(copy).has_value();
    };
    QVERIFY(broken("version", 2));
    QVERIFY(broken("step", "teleporting"));
    QVERIFY(broken("leaseId", "lease 1"));
    QVERIFY(broken("attempt", 4));
    QVERIFY(broken("runsOn", "gpu"));
    QVERIFY(broken("outcome", "released"));
    QVERIFY(broken("close", QJsonValue::Undefined));
    QVERIFY(broken("startedAt", "yesterday"));
    QVERIFY(broken("leaseExpiresAt", QJsonValue::Undefined));
}

void RunnerDeliveryRestartTest::journalWriteFailures() {
    { // Before acquire: no call, and no job any more.
        Host host;
        host.journal.failSteps = {int(Step::Acquiring)};
        host.delivery->heartbeatAnswered(true, false);
        QVERIFY(host.relay.calls.isEmpty());
        QCOMPARE(host.reports.size(), 1);
        QCOMPARE(host.heartbeatState(), QStringLiteral("draining"));
        host.journal.failSteps.clear();
        host.delivery->heartbeatAnswered(true, false);
        host.delivery->jobAvailable(QStringLiteral("job-1"));
        QVERIFY(host.relay.calls.isEmpty()); // until the process restarts
        QCOMPARE(host.reports.size(), 1);    // reported once
    }
    { // With the lease, before the session: released, never seen.
        Host host;
        host.journal.failSteps = {int(Step::Leased)};
        host.hold();
        QCOMPARE(host.last().kind, Kind::Release);
        QCOMPARE(host.last().reason, QStringLiteral("draining"));
        QVERIFY(host.session.opened.isEmpty());
        QCOMPARE(host.heartbeatState(), QStringLiteral("draining"));
    }
    { // Before the close: sent anyway.
        Host host;
        host.start();
        host.journal.failSteps = {int(Step::Closing)};
        host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
        QCOMPARE(host.last().kind, Kind::Finish);
        QCOMPARE(host.reports.size(), 1);
        host.answerOk({});
        QCOMPARE(host.delivery->state(), State::Idle);
        host.delivery->heartbeatAnswered(true, false);
        QCOMPARE(host.last().kind, Kind::Finish); // no acquire after a failed write
    }
    { // When the spec is taken: the start is refused and the lease released.
        Host host;
        host.journal.failSteps = {int(Step::SpecAccepted)};
        host.hold();
        host.delivery->lease().start();
        QCOMPARE(host.session.log, QStringList{"start refused lease_lost"});
        QCOMPARE(host.callKinds(), (QStringList{"acquire", "release"}));
        QCOMPARE(host.last().reason, QStringLiteral("draining"));
    }
}

void RunnerDeliveryRestartTest::restartWhileAcquiring() {
    Host host;
    host.delivery->restore(record(Step::Acquiring));
    QCOMPARE(host.last().kind, Kind::Acquire);
    QCOMPARE(host.last().acquireId, QStringLiteral("acq-7"));
    QCOMPARE(host.journal.writes, 0); // the record on disk already says so
    host.answerOk(leaseBody());
    QCOMPARE(host.session.opened.size(), 1);
}

void RunnerDeliveryRestartTest::restartWithALeaseAndNoSpec() {
    Host host;
    host.delivery->restore(record(Step::Leased));
    QCOMPARE(host.session.opened.size(), 1);
    QCOMPARE(host.session.opened.first().leaseId, QStringLiteral("lease-1"));
    QCOMPARE(host.session.opened.first().machineId, QStringLiteral("desktop-1"));
    QCOMPARE(host.last().kind, Kind::Renew); // renewed at once
    QCOMPARE(host.delivery->state(), State::Held);
    host.answerOk({});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 60'000);
    host.delivery->lease().start();
    QCOMPARE(host.last().kind, Kind::Start);
}

void RunnerDeliveryRestartTest::restartWithAnUntrustedLease() {
    Host host;
    host.trust.pinned.clear(); // unpaired while the runner was down
    host.delivery->restore(record(Step::Leased));
    QVERIFY(host.session.opened.isEmpty());
    QCOMPARE(host.callKinds(), (QStringList{"release", "renew"}));
    QCOMPARE(host.relay.calls.first().reason, QStringLiteral("untrusted_requester"));
}

void RunnerDeliveryRestartTest::restartAfterTheSpec_data() {
    QTest::addColumn<int>("step");
    QTest::addColumn<QString>("outcome");
    QTest::addColumn<QString>("reason");
    QTest::newRow("spec accepted") << int(Step::SpecAccepted) << "interrupted"
                                   << "runner_restarted";
    QTest::newRow("started") << int(Step::Started) << "interrupted" << "runner_restarted";
    QTest::newRow("closing") << int(Step::Closing) << "succeeded" << "exited";
}

void RunnerDeliveryRestartTest::restartAfterTheSpec() {
    QFETCH(int, step);
    QFETCH(QString, outcome);
    QFETCH(QString, reason);
    Host host;
    host.delivery->restore(record(Step(step)));
    QVERIFY(host.session.opened.isEmpty()); // no session: the command never runs again
    QCOMPARE(host.callKinds(), (QStringList{"finish", "renew"}));
    const LeaseCall finish = host.relay.calls.first();
    QCOMPARE(finish.outcome, outcome);
    QCOMPARE(finish.reason, reason);
    QCOMPARE(host.currentJob().toObject().value("leaseId").toString(), QStringLiteral("lease-1"));
    host.delivery->answered(finish,
                            {CallResult::Kind::Refused, QStringLiteral("lease_closed"), {}});
    QCOMPARE(host.delivery->state(), State::Idle);
    QCOMPARE(host.relay.calls.size(), 2); // once, its refusal ignored
    QVERIFY(!host.journal.current);
}

void RunnerDeliveryRestartTest::restartPolicyIsReplaceable() {
    class Reconnected final : public RestartPolicy {
      public:
        [[nodiscard]] Finish afterRestart(const JournalRecord&) const override {
            return {false, QStringLiteral("failed"), QStringLiteral("executor_lost")};
        }
    } policy;
    Host host(&policy);
    host.delivery->restore(record(Step::Started));
    QCOMPARE(host.relay.calls.first().outcome, QStringLiteral("failed"));
    QCOMPARE(host.relay.calls.first().reason, QStringLiteral("executor_lost"));
}

void RunnerDeliveryRestartTest::drainReleasesALeaseWithoutSpec() {
    Host host;
    host.hold();
    host.delivery->drain();
    QCOMPARE(host.session.log, QStringList{"withdraw"});
    QCOMPARE(host.last().kind, Kind::Release);
    QCOMPARE(host.last().reason, QStringLiteral("draining"));
    QCOMPARE(host.heartbeatState(), QStringLiteral("draining"));
    QVERIFY(host.currentJob().isObject()); // until the release is answered
    host.answerOk({{QStringLiteral("requeued"), true}});
    QVERIFY(host.currentJob().isNull());
    host.delivery->heartbeatAnswered(true, false);
    QCOMPARE(host.last().kind, Kind::Release); // no new acquire
}

void RunnerDeliveryRestartTest::drainLetsAStartedJobFinish() {
    Host host;
    host.hold();
    host.session.currentPhase = QStringLiteral("assigned");
    host.session.withdrawable = false; // the spec came
    host.delivery->drain();
    QCOMPARE(host.callKinds(), QStringList{"acquire"});
    host.delivery->lease().start();
    host.answerOk({{QStringLiteral("startedAt"), startedAt}});
    QCOMPARE(host.delivery->state(), State::Running);
    host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
    QCOMPARE(host.last().kind, Kind::Finish);
}

void RunnerDeliveryRestartTest::drainWhileAcquiring() {
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    host.delivery->drain();
    host.lose();
    host.advance(1'000); // still asked until a real answer
    QCOMPARE(host.callKinds(), (QStringList{"acquire", "acquire"}));
    host.answerOk(leaseBody());
    QCOMPARE(host.last().kind, Kind::Release);
    QCOMPARE(host.last().reason, QStringLiteral("draining"));
    QVERIFY(host.session.opened.isEmpty());
}

void RunnerDeliveryRestartTest::heartbeatInEachPhase() {
    Host host;
    QCOMPARE(host.heartbeatState(), QStringLiteral("idle"));
    QVERIFY(host.currentJob().isNull());
    host.delivery->heartbeatAnswered(true, false);
    QCOMPARE(host.heartbeatState(), QStringLiteral("idle")); // no lease known yet
    QVERIFY(host.currentJob().isNull());
    host.answerOk(leaseBody());
    QCOMPARE(host.heartbeatState(), QStringLiteral("busy"));
    const auto job = [&] { return host.currentJob().toObject(); };
    QCOMPARE(job().value("jobId").toString(), QStringLiteral("job-1"));
    QCOMPARE(job().value("leaseId").toString(), QStringLiteral("lease-1"));
    QCOMPARE(job().value("phase").toString(), QStringLiteral("assigned"));
    host.delivery->lease().start();
    QCOMPARE(job().value("phase").toString(), QStringLiteral("assigned"));
    host.answerOk({{QStringLiteral("startedAt"), startedAt}});
    host.session.currentPhase = QStringLiteral("syncing");
    QCOMPARE(job().value("phase").toString(), QStringLiteral("syncing"));
    host.session.currentPhase = QStringLiteral("running");
    QCOMPARE(job().value("phase").toString(), QStringLiteral("running"));
    host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
    QCOMPARE(host.heartbeatState(), QStringLiteral("busy")); // until the finish is answered
    QCOMPARE(job().value("phase").toString(), QStringLiteral("running"));
    host.answerOk({});
    QCOMPARE(host.heartbeatState(), QStringLiteral("idle"));
    QVERIFY(host.currentJob().isNull());
}

void RunnerDeliveryRestartTest::storageFailureStopsTakingJobs() {
    Host host;
    host.delivery->storageFailed(QStringLiteral("journal.json is corrupt"));
    host.delivery->storageFailed(QStringLiteral("accepted.json is corrupt"));
    QCOMPARE(host.reports, QStringList{"journal.json is corrupt"});
    QCOMPARE(host.heartbeatState(), QStringLiteral("draining"));
    host.delivery->heartbeatAnswered(true, false);
    host.delivery->jobAvailable(QStringLiteral("job-1"));
    QVERIFY(host.relay.calls.isEmpty());
}

void RunnerDeliveryRestartTest::effectiveLabelsAddTheSystemLabels() {
    QCOMPARE(effectiveLabels({QStringLiteral("gpu")}, QStringLiteral("arm64")),
             (QStringList{"gpu", "os:linux", "arch:arm64"}));
    QCOMPARE(effectiveLabels({}, QStringLiteral("x86_64")),
             (QStringList{"os:linux", "arch:x86_64"}));
}

} // namespace runner_delivery_test
