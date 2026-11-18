#include "RunnerDeliveryTest.h"

#include <QTest>

namespace runner_delivery_test {

using State = JobDelivery::State;
using Kind = LeaseCall::Kind;

namespace {

QList<LeaseCall> callsOf(const Host& host, const Kind kind) {
    QList<LeaseCall> out;
    for (const LeaseCall& call : host.relay.calls)
        if (call.kind == kind)
            out.append(call);
    return out;
}

} // namespace

void RunnerDeliveryLeaseTest::renewsEverySixtySeconds() {
    Host host;
    host.hold();
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 60'000);
    host.advance(59'999);
    QVERIFY(callsOf(host, Kind::Renew).isEmpty());
    host.advance(1);
    QCOMPARE(host.last().kind, Kind::Renew);
    QCOMPARE(host.last().leaseId, QStringLiteral("lease-1"));
    QCOMPARE(host.last().phase, QStringLiteral("assigned"));
    host.answerOk({{QStringLiteral("leaseExpiresAt"), QStringLiteral("2026-11-18T09:06:00Z")}});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 60'000);
    // The phase follows the session.
    host.session.currentPhase = QStringLiteral("syncing");
    host.advance(60'000);
    QCOMPARE(host.last().phase, QStringLiteral("syncing"));
    host.answerOk({});
    host.session.currentPhase = QStringLiteral("running");
    host.advance(60'000);
    QCOMPARE(host.last().phase, QStringLiteral("running"));
    QCOMPARE(callsOf(host, Kind::Renew).size(), 3);
}

void RunnerDeliveryLeaseTest::renewalRefusalsReachTheSession_data() {
    QTest::addColumn<QString>("code");
    QTest::addColumn<QString>("lost");
    QTest::newRow("lease_expired") << "lease_expired" << "lost lease_expired";
    QTest::newRow("entitlement_required") << "entitlement_required" << "lost entitlement_required";
    QTest::newRow("job_cancelled") << "job_cancelled" << "lost job_cancelled not_paired";
    QTest::newRow("unsupported") << "unsupported" << "lost unsupported";
}

void RunnerDeliveryLeaseTest::renewalRefusalsReachTheSession() {
    QFETCH(QString, code);
    QFETCH(QString, lost);
    Host host;
    host.start();
    host.session.currentPhase = QStringLiteral("running");
    host.advance(60'000);
    const QJsonObject job{{QStringLiteral("jobId"), QStringLiteral("job-1")},
                          {QStringLiteral("state"), QStringLiteral("failed")},
                          {QStringLiteral("reason"), QStringLiteral("not_paired")}};
    host.refuse(code, code == QStringLiteral("job_cancelled")
                          ? QJsonObject{{QStringLiteral("job"), job}}
                          : QJsonObject{});
    QCOMPARE(host.session.log.last(), lost);
    QVERIFY(!host.clock.deadlines.contains(DeliveryTimer::Renew));
    host.advance(10 * 60'000);
    QCOMPARE(callsOf(host, Kind::Renew).size(), 1);   // renewals stopped
    QCOMPARE(host.delivery->state(), State::Running); // the session finishes the lease
    host.delivery->lease().finish(QStringLiteral("interrupted"), QStringLiteral("lease_lost"));
    QCOMPARE(host.last().kind, Kind::Finish);
}

void RunnerDeliveryLeaseTest::renewalFailuresBackOff_data() {
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("code");
    QTest::newRow("network or 5xx") << int(CallResult::Kind::Lost) << QString();
    QTest::newRow("rate_limited") << int(CallResult::Kind::Refused) << "rate_limited";
    QTest::newRow("invalid_access after a new token")
        << int(CallResult::Kind::Refused) << "invalid_access";
}

void RunnerDeliveryLeaseTest::renewalFailuresBackOff() {
    QFETCH(int, kind);
    QFETCH(QString, code);
    Host host;
    host.start();
    host.advance(60'000);
    host.answer({CallResult::Kind(kind), code, {}});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 1'000);
    host.advance(1'000);
    host.answer({CallResult::Kind(kind), code, {}});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 2'000);
    host.advance(2'000);
    host.answerOk({});
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 60'000);
    QCOMPARE(callsOf(host, Kind::Renew).size(), 3);
    QCOMPARE(host.session.log, (QStringList{"started " + startedAt})); // no loss reported
    QCOMPARE(host.delivery->state(), State::Running);
}

void RunnerDeliveryLeaseTest::lostCredentialKeepsTheJobRunning() {
    Host host;
    host.start();
    host.advance(60'000);
    host.answer({CallResult::Kind::CredentialLost, {}, {}});
    QCOMPARE(host.reports.size(), 1);
    QCOMPARE(host.delivery->state(), State::Running);
    QCOMPARE(host.clock.remaining(DeliveryTimer::Renew), 1'000);
    QVERIFY(!host.delivery->takesJobs());
    host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
    host.answerOk({});
    host.delivery->heartbeatAnswered(true, false);
    QCOMPARE(host.last().kind, Kind::Finish); // no new acquire
}

void RunnerDeliveryLeaseTest::startAnswersTheSession() {
    Host host;
    host.hold();
    host.delivery->lease().start();
    QCOMPARE(host.last().kind, Kind::Start);
    QCOMPARE(host.last().leaseId, QStringLiteral("lease-1"));
    QCOMPARE(host.journal.steps(), (QStringList{"acquiring", "leased", "spec_accepted"}));
    QCOMPARE(host.relay.journalWritesAtSend.last(), 3); // journaled before the call
    QVERIFY(host.session.log.isEmpty());
    host.answerOk({{QStringLiteral("startedAt"), startedAt}});
    QCOMPARE(host.session.log, (QStringList{"started " + startedAt}));
    QCOMPARE(host.journal.current->startedAt, startedAt);
    QCOMPARE(host.journal.steps().last(), QStringLiteral("started"));
    QCOMPARE(host.delivery->state(), State::Running);
}

void RunnerDeliveryLeaseTest::startRefusals_data() {
    QTest::addColumn<QString>("code");
    QTest::addColumn<QStringList>("after"); // calls after the start
    QTest::newRow("lease_expired") << "lease_expired" << QStringList{};
    QTest::newRow("job_cancelled") << "job_cancelled" << QStringList{};
    QTest::newRow("unsupported") << "unsupported" << QStringList{"finish"};
    QTest::newRow("entitlement_required") << "entitlement_required" << QStringList{"finish"};
}

void RunnerDeliveryLeaseTest::startRefusals() {
    QFETCH(QString, code);
    QFETCH(QStringList, after);
    Host host;
    host.hold();
    // As the job session does: unsupported ends the job as rejected (runner_unavailable).
    host.session.onStartRefused = [&](const QString& refused) {
        host.session.currentPhase.clear();
        if (refused == QStringLiteral("unsupported"))
            host.delivery->lease().finish(QStringLiteral("rejected"),
                                          QStringLiteral("runner_unavailable"));
    };
    host.delivery->lease().start();
    host.refuse(code);
    QCOMPARE(host.session.log, QStringList{"start refused " + code});
    QCOMPARE(host.callKinds().mid(2), after); // after acquire and start
    if (!after.isEmpty()) {
        QCOMPARE(host.last().outcome, QStringLiteral("rejected"));
        QCOMPARE(host.last().reason, code == QStringLiteral("unsupported")
                                         ? QStringLiteral("runner_unavailable")
                                         : QStringLiteral("lease_lost"));
        host.answerOk({});
    }
    QCOMPARE(host.delivery->state(), State::Idle);
    QVERIFY(!host.journal.current);
}

void RunnerDeliveryLeaseTest::startWithoutAnswerIsAskedAgain() {
    Host host;
    host.hold();
    host.delivery->lease().start();
    host.lose();
    host.advance(1'000);
    QCOMPARE(host.last().kind, Kind::Start);
    host.refuse(QStringLiteral("rate_limited"));
    host.advance(2'000);
    host.answerOk({{QStringLiteral("startedAt"), startedAt}});
    QCOMPARE(callsOf(host, Kind::Start).size(), 3);
    QCOMPARE(host.session.log, (QStringList{"started " + startedAt}));
}

void RunnerDeliveryLeaseTest::finishIsSentOnceAndRepeatedUnchanged() {
    Host host;
    host.start();
    host.delivery->lease().finish(QStringLiteral("failed"), QStringLiteral("exited_nonzero"));
    QCOMPARE(host.journal.steps().last(), QStringLiteral("closing"));
    QCOMPARE(host.relay.journalWritesAtSend.last(), host.journal.writes);
    // Another body for the same lease is never sent.
    host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
    host.delivery->lease().release(QStringLiteral("draining"));
    QCOMPARE(callsOf(host, Kind::Finish).size(), 1);
    QVERIFY(callsOf(host, Kind::Release).isEmpty());
    host.lose();
    host.advance(1'000);
    host.refuse(QStringLiteral("rate_limited"));
    host.advance(2'000);
    const QList<LeaseCall> finishes = callsOf(host, Kind::Finish);
    QCOMPARE(finishes.size(), 3);
    for (const LeaseCall& call : finishes) {
        QCOMPARE(call.outcome, QStringLiteral("failed"));
        QCOMPARE(call.reason, QStringLiteral("exited_nonzero"));
    }
    host.answerOk({});
    QCOMPARE(host.delivery->state(), State::Idle);
    QVERIFY(!host.journal.current);
    QVERIFY(host.clock.deadlines.isEmpty()); // renewals end with the lease
}

void RunnerDeliveryLeaseTest::closeEndsOnTheseAnswers_data() {
    QTest::addColumn<bool>("release");
    QTest::addColumn<QString>("code");
    QTest::newRow("finish lease_expired") << false << "lease_expired";
    QTest::newRow("finish lease_closed") << false << "lease_closed";
    QTest::newRow("finish unsupported") << false << "unsupported";
    QTest::newRow("release lease_expired") << true << "lease_expired";
    QTest::newRow("release lease_closed") << true << "lease_closed";
    QTest::newRow("release job_started") << true << "job_started";
}

void RunnerDeliveryLeaseTest::closeEndsOnTheseAnswers() {
    QFETCH(bool, release);
    QFETCH(QString, code);
    Host host;
    host.hold();
    if (release)
        host.delivery->lease().release(QStringLiteral("peer_unreachable"));
    else
        host.delivery->lease().finish(QStringLiteral("rejected"), QStringLiteral("spec_invalid"));
    const int sent = host.relay.calls.size();
    host.refuse(code);
    host.advance(10 * 60'000);
    QCOMPARE(host.relay.calls.size(), sent); // nothing more, no error to the session
    QVERIFY(host.session.log.isEmpty());
    QCOMPARE(host.delivery->state(), State::Idle);
}

void RunnerDeliveryLeaseTest::releaseFromTheSession() {
    Host host;
    host.hold();
    host.delivery->lease().release(QStringLiteral("peer_unreachable"));
    QCOMPARE(host.last().kind, Kind::Release);
    QCOMPARE(host.last().reason, QStringLiteral("peer_unreachable"));
    QCOMPARE(host.journal.current->release, true);
    host.answerOk({{QStringLiteral("requeued"), true}});
    QCOMPARE(host.delivery->state(), State::Idle);
}

void RunnerDeliveryLeaseTest::nextJobAfterTheClose() {
    Host host;
    host.start();
    host.delivery->lease().finish(QStringLiteral("succeeded"), QStringLiteral("exited"));
    host.answerOk({});
    host.delivery->heartbeatAnswered(true, false);
    QCOMPARE(host.last().kind, Kind::Acquire);
    QCOMPARE(host.last().acquireId, QStringLiteral("acq-2"));
    host.answerOk(leaseBody(QStringLiteral("job-2"), QStringLiteral("lease-2")));
    QCOMPARE(host.session.opened.last().jobId, QStringLiteral("job-2"));
}

} // namespace runner_delivery_test
