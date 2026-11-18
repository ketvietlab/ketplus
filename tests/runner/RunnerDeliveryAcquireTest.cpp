#include "RunnerDeliveryTest.h"

#include <QTest>

namespace runner_delivery_test {

using State = JobDelivery::State;
using Kind = LeaseCall::Kind;

void RunnerDeliveryAcquireTest::acquiresOnlyWhenIdleAndTakingJobs() {
    Host host;
    host.delivery->heartbeatAnswered(false, false); // nothing pending
    QVERIFY(host.relay.calls.isEmpty());
    host.delivery->heartbeatAnswered(true, true); // disabled
    QVERIFY(host.relay.calls.isEmpty());
    host.delivery->heartbeatAnswered(true, false);
    QCOMPARE(host.callKinds(), QStringList{"acquire"});
    QVERIFY(host.last().jobId.isEmpty()); // the next matching job
    QCOMPARE(host.delivery->state(), State::Acquiring);
    // Not idle: other triggers wait.
    host.delivery->heartbeatAnswered(true, false);
    host.delivery->jobAvailable(QStringLiteral("job-2"));
    QCOMPARE(host.relay.calls.size(), 1);

    Host draining;
    draining.delivery->drain();
    draining.delivery->heartbeatAnswered(true, false);
    draining.delivery->jobAvailable(QStringLiteral("job-1"));
    QVERIFY(draining.relay.calls.isEmpty());
    QCOMPARE(draining.heartbeatState(), QStringLiteral("draining"));
}

void RunnerDeliveryAcquireTest::jobAvailableAsksForThatJob() {
    Host host;
    host.delivery->jobAvailable(QStringLiteral("job-7"));
    QCOMPARE(host.last().kind, Kind::Acquire);
    QCOMPARE(host.last().jobId, QStringLiteral("job-7"));
    QCOMPARE(host.journal.current->wantedJobId, QStringLiteral("job-7"));
}

void RunnerDeliveryAcquireTest::acquireIdIsJournaledFirst() {
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    QCOMPARE(host.relay.journalWritesAtSend.first(), 1);
    QCOMPARE(host.journal.steps(), QStringList{"acquiring"});
    QCOMPARE(host.journal.current->acquireId, host.last().acquireId);
    QCOMPARE(host.last().acquireId, QStringLiteral("acq-1"));
}

void RunnerDeliveryAcquireTest::lostAnswerAsksAgainWithTheSameId_data() {
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("code");
    QTest::newRow("network or 5xx") << int(CallResult::Kind::Lost) << QString();
    QTest::newRow("rate_limited") << int(CallResult::Kind::Refused) << "rate_limited";
}

void RunnerDeliveryAcquireTest::lostAnswerAsksAgainWithTheSameId() {
    QFETCH(int, kind);
    QFETCH(QString, code);
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    host.answer({CallResult::Kind(kind), code, {}});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Retry), 1'000);
    host.advance(999);
    QCOMPARE(host.relay.calls.size(), 1);
    host.advance(1);
    QCOMPARE(host.relay.calls.size(), 2);
    QCOMPARE(host.last().acquireId, QStringLiteral("acq-1"));
    host.answer({CallResult::Kind(kind), code, {}});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Retry), 2'000); // doubled
    host.advance(2'000);
    host.answerOk(leaseBody());
    QCOMPARE(host.delivery->state(), State::Held);
    QCOMPARE(host.journal.writes, 2); // one acquireId for every attempt, then the lease
}

void RunnerDeliveryAcquireTest::refusalsGoIdleWithoutRetrying_data() {
    QTest::addColumn<QString>("code");
    for (const char* code : {"job_unavailable", "not_paired", "label_mismatch", "runner_disabled",
                             "entitlement_required"})
        QTest::newRow(code) << QString::fromLatin1(code);
}

void RunnerDeliveryAcquireTest::refusalsGoIdleWithoutRetrying() {
    QFETCH(QString, code);
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    host.refuse(code);
    QCOMPARE(host.delivery->state(), State::Idle);
    QVERIFY(!host.journal.current);
    QVERIFY(host.clock.deadlines.isEmpty());
    host.advance(10 * 60'000);
    QCOMPARE(host.relay.calls.size(), 1); // no retry of its own
    QVERIFY(host.session.opened.isEmpty());
    // The next trigger asks again with a new acquireId; a disabled runner first needs a
    // heartbeat that says it is enabled.
    host.delivery->jobAvailable(QStringLiteral("job-2"));
    if (code == QStringLiteral("runner_disabled")) {
        QCOMPARE(host.relay.calls.size(), 1);
        host.delivery->heartbeatAnswered(true, false);
    }
    QCOMPARE(host.relay.calls.size(), 2);
    QCOMPARE(host.last().acquireId, QStringLiteral("acq-2"));
}

void RunnerDeliveryAcquireTest::busyLeaseOfUnknownRequesterIsReleased() {
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    const QJsonObject held{
        {QStringLiteral("jobId"), QStringLiteral("job-9")},
        {QStringLiteral("leaseId"), QStringLiteral("lease-9")},
        {QStringLiteral("attempt"), 2},
        {QStringLiteral("leaseExpiresAt"), QStringLiteral("2026-11-18T09:05:00Z")}};
    host.refuse(QStringLiteral("runner_busy"), {{QStringLiteral("lease"), held}});
    QCOMPARE(host.last().kind, Kind::Release);
    QCOMPARE(host.last().leaseId, QStringLiteral("lease-9"));
    QCOMPARE(host.last().reason, QStringLiteral("untrusted_requester"));
    QVERIFY(host.session.opened.isEmpty());
    QCOMPARE(host.journal.steps(), (QStringList{"acquiring", "leased", "closing"}));
    QCOMPARE(host.currentJob().toObject().value("jobId").toString(), QStringLiteral("job-9"));
    host.answerOk({{QStringLiteral("requeued"), true}});
    QCOMPARE(host.delivery->state(), State::Idle);
    QVERIFY(!host.journal.current);
}

void RunnerDeliveryAcquireTest::busyLeaseThatWasStartedIsFinished() {
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    host.refuse(QStringLiteral("runner_busy"),
                {{QStringLiteral("lease"),
                  QJsonObject{{QStringLiteral("jobId"), QStringLiteral("job-9")},
                              {QStringLiteral("leaseId"), QStringLiteral("lease-9")},
                              {QStringLiteral("attempt"), 1},
                              {QStringLiteral("leaseExpiresAt"),
                               QStringLiteral("2026-11-18T09:05:00Z")}}}});
    host.refuse(QStringLiteral("job_started"));
    QCOMPARE(host.last().kind, Kind::Finish);
    QCOMPARE(host.last().outcome, QStringLiteral("interrupted"));
    QCOMPARE(host.last().reason, QStringLiteral("runner_restarted"));
    host.refuse(QStringLiteral("lease_closed")); // ignored
    QCOMPARE(host.delivery->state(), State::Idle);
    QCOMPARE(host.callKinds(), (QStringList{"acquire", "release", "finish"}));
}

void RunnerDeliveryAcquireTest::sessionGetsATrustedLease() {
    Host host;
    host.hold();
    QCOMPARE(host.delivery->state(), State::Held);
    QCOMPARE(host.session.opened.size(), 1);
    const HeldLease lease = host.session.opened.first();
    QCOMPARE(lease.jobId, QStringLiteral("job-1"));
    QCOMPARE(lease.attempt, 1);
    QCOMPARE(lease.leaseId, QStringLiteral("lease-1"));
    QCOMPARE(lease.machineId, QStringLiteral("desktop-1"));
    QCOMPARE(lease.workspaceId, QStringLiteral("ws-1"));
    QCOMPARE(host.journal.steps(), (QStringList{"acquiring", "leased"}));
    QCOMPARE(host.journal.current->lease.runsOn, QStringList{"gpu"});
    // Every label of the runner, system ones included.
    Host system;
    system.hold({QStringLiteral("os:linux"), QStringLiteral("arch:x86_64"), QStringLiteral("gpu")});
    QCOMPARE(system.session.opened.size(), 1);
}

void RunnerDeliveryAcquireTest::untrustedRequesterIsReleased() {
    Host host;
    host.trust.pinned.clear(); // no active pin for desktop-1
    host.hold();
    QCOMPARE(host.last().kind, Kind::Release);
    QCOMPARE(host.last().reason, QStringLiteral("untrusted_requester"));
    QVERIFY(host.session.opened.isEmpty());
    host.answerOk({{QStringLiteral("requeued"), true}});
    QCOMPARE(host.delivery->state(), State::Idle);
}

void RunnerDeliveryAcquireTest::labelsOutsideTheRunnerAreReleased() {
    for (const QString& label : {QStringLiteral("cuda"), QStringLiteral("arch:arm64")}) {
        Host host;
        host.hold({QStringLiteral("gpu"), label});
        QCOMPARE(host.last().kind, Kind::Release);
        QCOMPARE(host.last().reason, QStringLiteral("label_mismatch"));
        QVERIFY(host.session.opened.isEmpty());
    }
}

void RunnerDeliveryAcquireTest::unreadableLeaseIsAskedAgain() {
    Host host;
    host.delivery->heartbeatAnswered(true, false);
    QJsonObject body = leaseBody();
    body.remove(QStringLiteral("requester"));
    host.answerOk(body);
    QVERIFY(host.session.opened.isEmpty());
    host.advance(1'000);
    QCOMPARE(host.last().kind, Kind::Acquire);
    QCOMPARE(host.last().acquireId, QStringLiteral("acq-1"));
}

} // namespace runner_delivery_test
