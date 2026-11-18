#include "RunnerRunTest.h"

#include <QJsonArray>
#include <QTest>

namespace runner_run_test {

using delivery::LeaseCall;
using Phase = session::JobSession::Phase;

namespace {

// A job held, connected, its spec taken and its snapshot ready: the command runs.
void runJob(Run& run, const QString& jobId = QStringLiteral("job_42")) {
    run.acquire(jobId);
    run.connectDesktop();
    run.deliver(makeSpec(jobId));
    QCOMPARE(run.receiver.begun.size() > 0, true);
    run.receiver.ready(QStringLiteral("/work/space"), {});
    run.settle();
}

void exitWith(Run& run, const qint64 code) {
    session::CommandExit exit;
    exit.exitCode = code;
    exit.cleanupStatus = QStringLiteral("verified");
    run.command.events.finished(exit);
    run.settle();
}

} // namespace

void RunnerRunJobTest::wholeJobThenTheNextOne() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    QCOMPARE(run.command.launches.size(), 1);
    const session::LaunchRequest& launch = run.command.launches.first();
    QCOMPARE(launch.jobId, QStringLiteral("job_42"));
    QCOMPARE(launch.workspace, QStringLiteral("/work/space"));
    QCOMPARE(launch.environment.value(QStringLiteral("PATH")), QStringLiteral("/usr/bin:/bin"));
    QCOMPARE(launch.environment.value(QStringLiteral("SECRET_VALUE")),
             QString::fromLatin1(commandMarker));
    QCOMPARE(run.loop->session().phase(), Phase::Running);
    QCOMPARE(run.journalRecord()->step, delivery::JournalRecord::Step::Started);
    QVERIFY(run.sentTypes().contains(QStringLiteral("job.accepted")));
    QVERIFY(run.sentTypes().contains(QStringLiteral("job.started")));

    run.advance(heartbeatSpacingMs);
    QCOMPARE(run.heartbeatState(), QStringLiteral("busy"));
    QCOMPARE(run.relay.heartbeats.last().value("currentJob").toObject().value("phase").toString(),
             QStringLiteral("running"));

    run.command.events.output("built\n");
    exitWith(run, 0);
    QVERIFY(run.sentTypes().contains(QStringLiteral("job.result")));
    QCOMPARE(run.loop->session().phase(), Phase::Delivering);
    QVERIFY(!run.leaseKinds().join(',').contains(QStringLiteral("finish")));
    run.deliver(protocol::JobResultAck{7, QStringLiteral("job_42")});
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "start", "finish succeeded exited"}));
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
    QVERIFY(!run.journalRecord());
    QCOMPARE(run.receiver.idles, 1);

    // The next job.
    run.acquire(QStringLiteral("job_43"));
    QCOMPARE(run.leaseKinds().last(), QStringLiteral("acquire"));
    QCOMPARE(run.relay.leases.last().jobId, QString());
    QCOMPARE(run.loop->session().phase(), Phase::Assigned);
}

void RunnerRunJobTest::unreachableDesktopReleasesPeerUnreachable() {
    Run run;
    run.start();
    run.socketReady();
    run.acquire();
    run.advance(60'000);
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "release peer_unreachable"}));
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
    QVERIFY(!run.exit);
}

void RunnerRunJobTest::unpinnedDesktopReleasesUntrustedRequester() {
    Run run;
    run.start();
    run.socketReady();
    run.relay.pending = true;
    run.relay.leaseAnswer = [](const LeaseCall& call) {
        if (call.kind == LeaseCall::Kind::Acquire)
            return ok(leaseReply(QStringLiteral("job_42"), QStringLiteral("lease_1"),
                                 QStringLiteral("m_stranger")));
        return ok();
    };
    run.advance(heartbeatIntervalMs);
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "release untrusted_requester"}));
    QVERIFY(run.transports.made.isEmpty());
    QCOMPARE(run.loop->session().phase(), Phase::Idle);
}

// The command runner cannot contain the job when its spec comes: the spec is rejected and the
// lease finished, without a launch.
void RunnerRunJobTest::sessionRefusesTheSpec() {
    Run run;
    run.start();
    run.socketReady();
    run.acquire();
    run.connectDesktop();
    run.command.state = CommandHost::Readiness::Unavailable; // not told yet
    run.deliver(makeSpec());
    QVERIFY(run.command.launches.isEmpty());
    QVERIFY(run.sentTypes().contains(QStringLiteral("job.rejected")));
    QVERIFY(run.leaseKinds().last().startsWith(QStringLiteral("finish rejected")));
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
}

void RunnerRunJobTest::reconnectAfterALostConnection() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    const auto starts = [&run] {
        int n = 0;
        for (const QJsonObject& message : run.socket->sent)
            n += message.value("type").toString() == QStringLiteral("signal.start");
        return n;
    };
    QCOMPARE(starts(), 1);
    run.transports.made.last()->events->closed();
    run.settle();
    run.advance(10'000);
    QCOMPARE(starts(), 2);
    QCOMPARE(run.transports.made.size(), 1); // made when the Relay says started
    run.connectionId = QStringLiteral("pc_bcdefghijklmnopqrstuvwxy");
    run.wakeup({{"type", "signal.started"},
                {"connectionId", run.connectionId},
                {"iceServers", QJsonArray{}}});
    QCOMPARE(run.transports.made.size(), 2);
    QCOMPARE(run.command.kills, 0);
    QCOMPARE(run.loop->session().phase(), Phase::Running);
}

void RunnerRunJobTest::failedRenewalStopsTheJob() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    run.relay.leaseAnswer = [](const LeaseCall& call) {
        return call.kind == LeaseCall::Kind::Renew ? refused(QStringLiteral("lease_expired"))
                                                   : ok();
    };
    run.advance(delivery::renewIntervalMs);
    QCOMPARE(run.command.kills, 1);
    exitWith(run, 137);
    // The lease is already gone: the finish is the delivery's last word, and nothing follows.
    QCOMPARE(run.leaseKinds().last(), QStringLiteral("finish interrupted lease_lost"));
    QCOMPARE(count(run.leaseKinds(), "renew"), 1);
    run.advance(delivery::renewIntervalMs * 2);
    QCOMPARE(count(run.leaseKinds(), "renew"), 1);
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
    QVERIFY(!run.exit);
}

// The Relay socket only brings wake-ups and signalling: losing it stops nothing.
void RunnerRunJobTest::lostRelaySocketKeepsTheJob() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    run.loop->socketClosed(1006);
    run.settle();
    run.advance(heartbeatIntervalMs);
    QCOMPARE(run.command.kills, 0);
    QCOMPARE(run.loop->session().phase(), Phase::Running);
    QCOMPARE(run.heartbeatState(), QStringLiteral("busy"));
    QVERIFY(!run.exit);
}

void RunnerRunJobTest::desktopCancel() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    run.deliver(protocol::JobCancel{7, QStringLiteral("job_42"), QStringLiteral("user")});
    QCOMPARE(run.command.kills, 1);
    session::CommandExit exit;
    exit.signal = 15;
    exit.cleanupStatus = QStringLiteral("verified");
    run.command.events.finished(exit);
    run.settle();
    run.deliver(protocol::JobResultAck{7, QStringLiteral("job_42")});
    QCOMPARE(run.leaseKinds().last(), QStringLiteral("finish cancelled user"));
}

void RunnerRunJobTest::jobTimeout() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    run.advance(600'000 - 1);
    QCOMPARE(run.command.kills, 0);
    run.advance(1);
    QCOMPARE(run.command.kills, 1);
    session::CommandExit exit;
    exit.signal = 9;
    exit.cleanupStatus = QStringLiteral("verified");
    run.command.events.finished(exit);
    run.settle();
    run.deliver(protocol::JobResultAck{7, QStringLiteral("job_42")});
    QCOMPARE(run.leaseKinds().last(), QStringLiteral("finish failed timed_out"));
}

} // namespace runner_run_test
