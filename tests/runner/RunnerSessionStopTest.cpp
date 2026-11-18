#include "RunnerSessionTest.h"

#include <QTest>

namespace runner_session_test {

using namespace protocol;
using Phase = session::JobSession::Phase;

namespace {

const session::CommandExit killed{QStringLiteral("exited"), std::nullopt, 9, {}, "verified"};
const session::CommandExit exitedZero{QStringLiteral("exited"), 0, std::nullopt, {}, "verified"};

JobCancel cancelFrame(const QString& reason = QStringLiteral("user")) {
    return {7, QStringLiteral("job-1"), reason};
}

} // namespace

void RunnerSessionStopTest::cancelInEachPhase() {
    { // Before the spec: dropped, the lease finishes cancelled, nothing is sent.
        Host host;
        host.connect();
        host.clearSent();
        host.deliver(cancelFrame());
        QVERIFY(host.channel.sent.isEmpty());
        QCOMPARE(host.lease.calls, QStringList{"finish cancelled user"});
        QCOMPARE(host.session->phase(), Phase::Idle);
    }
    { // While the snapshot comes: the transfer stops, a cancelled result, then finish.
        Host host;
        host.accept(makeSpec());
        host.clearSent();
        host.deliver(cancelFrame(QStringLiteral("deadline")));
        QCOMPARE(host.snapshots.cancels, 1);
        QCOMPARE(host.sentTypes(), QStringList{"job.result"});
        const JobResult result = host.sentOf<JobResult>().first();
        QCOMPARE(result.state, QStringLiteral("cancelled"));
        QCOMPARE(result.reason, QStringLiteral("deadline"));
        QCOMPARE(result.cleanupStatus, QStringLiteral("verified"));
        QVERIFY(host.command.launches.isEmpty());
        host.deliver(JobResultAck{7, QStringLiteral("job-1")});
        QCOMPARE(host.lease.calls, (QStringList{"start", "finish cancelled deadline"}));
    }
    { // Running: killed; the result waits for the cleanup.
        Host host;
        host.launch(makeSpec());
        host.clearSent();
        host.deliver(cancelFrame());
        QCOMPARE(host.command.kills, 1);
        QVERIFY(host.sentOf<JobResult>().isEmpty());
        QCOMPARE(host.session->phase(), Phase::Stopping);
        host.session->commandFinished(killed);
        const JobResult result = host.sentOf<JobResult>().first();
        QCOMPARE(result.state, QStringLiteral("cancelled"));
        QCOMPARE(result.signal, 9);
        QVERIFY(!result.exitCode);
        QCOMPARE(result.startedAt, startedAt);
    }
    { // With a result: nothing changes.
        Host host;
        host.launch(makeSpec());
        host.session->commandFinished(exitedZero);
        host.clearSent();
        host.deliver(cancelFrame());
        QVERIFY(host.channel.sent.isEmpty());
        QCOMPARE(host.command.kills, 0);
        host.deliver(JobResultAck{7, QStringLiteral("job-1")});
        QCOMPARE(host.lease.calls, (QStringList{"start", "finish succeeded exited"}));
    }
}

void RunnerSessionStopTest::cancelWhileStartIsAsked() {
    Host host;
    host.connect();
    host.deliver(makeSpec());
    host.clearSent();
    host.deliver(cancelFrame());
    QVERIFY(host.channel.sent.isEmpty());
    host.session->leaseStarted(startedAt);
    QCOMPARE(host.sentTypes(), (QStringList{"job.accepted", "job.result"}));
    QCOMPARE(host.sentOf<JobResult>().first().state, QStringLiteral("cancelled"));
    QVERIFY(host.snapshots.begun.isEmpty());
    QVERIFY(host.command.launches.isEmpty());
}

void RunnerSessionStopTest::repeatedCancelChangesNothing() {
    Host host;
    host.launch(makeSpec());
    host.deliver(cancelFrame());
    host.deliver(cancelFrame(QStringLiteral("session_closed")));
    host.session->leaseLost(QStringLiteral("job_cancelled"));
    host.advance(60'000); // the deadline as well
    QCOMPARE(host.command.kills, 1);
    host.session->commandFinished(killed);
    host.deliver(cancelFrame());
    QCOMPARE(host.sentOf<JobResult>().size(), 1);
    QCOMPARE(host.sentOf<JobResult>().first().reason, QStringLiteral("user"));
}

void RunnerSessionStopTest::timeoutCountsFromStarted() {
    Host host;
    host.accept(makeSpec());   // timeoutMs 60,000
    host.advance(10 * 60'000); // syncing time does not count
    QCOMPARE(host.command.kills, 0);
    host.session->snapshotReady(QStringLiteral("/work/ws"), {});
    host.advance(59'999);
    QCOMPARE(host.command.kills, 0);
    host.advance(1);
    QCOMPARE(host.command.kills, 1);
    host.session->commandFinished(killed);
    const JobResult result = host.sentOf<JobResult>().first();
    QCOMPARE(result.state, QStringLiteral("timed_out"));
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    QCOMPARE(host.lease.calls.last(), QStringLiteral("finish failed timed_out"));
}

void RunnerSessionStopTest::resultAndCancelRace() {
    { // The command ends first: the cancel that follows changes nothing.
        Host host;
        host.launch(makeSpec());
        host.session->commandFinished({QStringLiteral("exited"), 2, std::nullopt, {}, "verified"});
        host.deliver(cancelFrame());
        QCOMPARE(host.command.kills, 0);
        QCOMPARE(host.sentOf<JobResult>().size(), 1);
        QCOMPARE(host.sentOf<JobResult>().first().state, QStringLiteral("exited"));
    }
    { // The cancel first: the exit that follows is reported as cancelled, once.
        Host host;
        host.launch(makeSpec());
        host.deliver(cancelFrame());
        host.session->commandFinished(exitedZero);
        host.session->commandFinished(exitedZero); // a stray second report
        QCOMPARE(host.sentOf<JobResult>().size(), 1);
        QCOMPARE(host.sentOf<JobResult>().first().state, QStringLiteral("cancelled"));
    }
}

void RunnerSessionStopTest::cleanupStatusIsKept() {
    Host host;
    host.launch(makeSpec());
    host.deliver(cancelFrame());
    host.session->commandFinished({QStringLiteral("exited"), std::nullopt, 9, {}, "unverified"});
    QCOMPARE(host.sentOf<JobResult>().first().cleanupStatus, QStringLiteral("unverified"));
}

void RunnerSessionStopTest::lostChannelReconnectsAndResumes() {
    Host host;
    host.launch(makeSpec());
    host.session->channelLost();
    QCOMPARE(host.clock.remaining(SessionTimer::Reconnect), 10'000);
    host.session->commandOutput("still running");
    host.advance(9'999);
    QCOMPARE(host.channel.connectionRequests, 1);
    host.advance(1);
    QCOMPARE(host.channel.connectionRequests, 2);
    host.advance(29'999);
    QCOMPARE(host.channel.connectionRequests, 2);
    host.advance(1);
    QCOMPARE(host.channel.connectionRequests, 3);
    host.advance(30'000);
    QCOMPARE(host.channel.connectionRequests, 4);
    host.clearSent();
    host.open(QStringLiteral("c2"));
    QCOMPARE(host.sentTypes(), QStringList{"hello"});
    host.welcome();
    host.deliver(JobResume{7, QStringLiteral("job-1"), 0});
    QCOMPARE(host.sentTypes(), (QStringList{"hello", "job.started", "job.output"}));
    QCOMPARE(host.sentOf<JobOutput>().first().data, QByteArray("still running"));
    QVERIFY(!host.clock.armed(SessionTimer::Reconnect));
    QVERIFY(!host.clock.armed(SessionTimer::PeerAbsent));
    QCOMPARE(host.command.launches.size(), 1);
    QCOMPARE(host.lease.calls, QStringList{"start"});
}

void RunnerSessionStopTest::peerAbsentFifteenMinutes() {
    { // Running.
        Host host;
        host.launch(makeSpec(QStringLiteral("job-1")));
        host.clock.disarm(SessionTimer::JobTimeout); // keep the deadline out of the way
        host.session->channelLost();
        host.advance(15 * 60'000 - 1);
        QCOMPARE(host.command.kills, 0);
        host.advance(1);
        QCOMPARE(host.command.kills, 1);
        host.session->commandFinished(killed);
        QCOMPARE(host.session->phase(), Phase::Delivering);
        // The desktop comes back within the hour and gets the result.
        host.advance(59 * 60'000);
        host.open(QStringLiteral("c2"));
        host.welcome();
        host.clearSent();
        host.deliver(JobResume{7, QStringLiteral("job-1"), 0});
        const JobResult result = host.sentOf<JobResult>().first();
        QCOMPARE(result.state, QStringLiteral("interrupted"));
        QCOMPARE(result.reason, QStringLiteral("requester_lost"));
        host.deliver(JobResultAck{7, QStringLiteral("job-1")});
        QCOMPARE(host.lease.calls, (QStringList{"start", "finish interrupted requester_lost"}));
    }
    { // A connection that opens resets the 15 minutes.
        Host host;
        host.launch(makeSpec());
        host.clock.disarm(SessionTimer::JobTimeout);
        host.session->channelLost();
        host.advance(14 * 60'000);
        host.open(QStringLiteral("c2"));
        host.session->channelLost();
        host.advance(14 * 60'000);
        QCOMPARE(host.command.kills, 0);
    }
    { // Syncing: the transfer stops, no command is killed.
        Host host;
        host.accept(makeSpec());
        host.session->channelLost();
        host.advance(15 * 60'000);
        QCOMPARE(host.snapshots.cancels, 1);
        QCOMPARE(host.session->phase(), Phase::Delivering);
    }
}

void RunnerSessionStopTest::resultHeldSixtyMinutes() {
    Host host;
    host.launch(makeSpec());
    host.session->channelLost();
    host.session->commandFinished(exitedZero);
    host.advance(60 * 60'000 - 1);
    QCOMPARE(host.session->phase(), Phase::Delivering);
    QCOMPARE(host.lease.calls, QStringList{"start"});
    host.advance(1);
    QCOMPARE(host.session->phase(), Phase::Idle);
    QCOMPARE(host.lease.calls, (QStringList{"start", "finish succeeded exited"}));
    // A desktop that comes back later finds nothing to resume.
    host.open(QStringLiteral("c3"));
    QCOMPARE(host.channel.closes, QStringList{"no_job"});
}

void RunnerSessionStopTest::leaseLosses_data() {
    QTest::addColumn<QString>("code");
    QTest::addColumn<QString>("reason");
    QTest::newRow("lease_expired") << "lease_expired" << "lease_lost";
    QTest::newRow("entitlement_required") << "entitlement_required" << "entitlement_required";
}

void RunnerSessionStopTest::leaseLosses() {
    QFETCH(QString, code);
    QFETCH(QString, reason);
    { // Running: killed, interrupted.
        Host host;
        host.launch(makeSpec());
        host.session->leaseLost(code);
        QCOMPARE(host.command.kills, 1);
        host.session->commandFinished(killed);
        const JobResult result = host.sentOf<JobResult>().first();
        QCOMPARE(result.state, QStringLiteral("interrupted"));
        QCOMPARE(result.reason, reason);
        // The result is sent once and not held: the lease is gone.
        QCOMPARE(host.lease.calls, (QStringList{"start", "finish interrupted " + reason}));
        QCOMPARE(host.session->phase(), Phase::Idle);
    }
    { // Before the spec: the job and its connection are dropped, nothing is finished.
        Host host;
        host.connect();
        host.clearSent();
        host.session->leaseLost(code);
        QVERIFY(host.channel.sent.isEmpty());
        QCOMPARE(host.channel.closes, QStringList{"lease_lost"});
        QVERIFY(host.lease.calls.isEmpty());
        QCOMPARE(host.session->phase(), Phase::Idle);
        QVERIFY(host.clock.deadlines.isEmpty());
    }
    { // While `job.start` is asked: the desktop learns lease_lost.
        Host host;
        host.connect();
        host.deliver(makeSpec());
        host.clearSent();
        host.session->leaseLost(code);
        QCOMPARE(host.sentOf<JobRejected>().first().code, QStringLiteral("lease_lost"));
        host.session->leaseStarted(startedAt); // too late
        QCOMPARE(host.sentTypes(), QStringList{"job.rejected"});
        QVERIFY(host.snapshots.begun.isEmpty());
    }
}

void RunnerSessionStopTest::unsupportedAfterStartRunsToTheEnd() {
    Host host;
    host.launch(makeSpec());
    host.session->leaseLost(QStringLiteral("unsupported"));
    QCOMPARE(host.command.kills, 0);
    host.session->commandOutput("done");
    host.session->commandFinished(exitedZero);
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    QCOMPARE(host.lease.calls, (QStringList{"start", "finish succeeded exited"}));
}

void RunnerSessionStopTest::relayCancelWithoutChannel() {
    Host host;
    host.launch(makeSpec());
    host.session->channelLost();
    host.session->leaseLost(QStringLiteral("job_cancelled"));
    QCOMPARE(host.command.kills, 1);
    host.session->commandFinished(killed);
    host.open(QStringLiteral("c2"));
    host.welcome();
    host.clearSent();
    host.deliver(JobResume{7, QStringLiteral("job-1"), 0});
    const JobResult result = host.sentOf<JobResult>().first();
    QCOMPARE(result.state, QStringLiteral("cancelled"));
    QCOMPARE(result.reason, QStringLiteral("cancelled")); // the Relay gave no reason
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    QCOMPARE(host.lease.calls, (QStringList{"start", "finish cancelled cancelled"}));

    // The Relay's own reason, when it fits.
    Host other;
    other.launch(makeSpec());
    other.session->leaseLost(QStringLiteral("job_cancelled"), QStringLiteral("owner_stopped"));
    other.session->commandFinished(killed);
    QCOMPARE(other.sentOf<JobResult>().first().reason, QStringLiteral("owner_stopped"));
    Host odd;
    odd.launch(makeSpec());
    odd.session->leaseLost(QStringLiteral("job_cancelled"), QStringLiteral("Not A Code"));
    odd.session->commandFinished(killed);
    QCOMPARE(odd.sentOf<JobResult>().first().reason, QStringLiteral("cancelled"));
}

void RunnerSessionStopTest::leaseLostWhileHoldingTheResult() {
    for (const QString& code : {QStringLiteral("lease_expired"), QStringLiteral("job_cancelled"),
                                QStringLiteral("entitlement_required")}) {
        Host host;
        host.launch(makeSpec());
        host.session->commandFinished(exitedZero);
        host.session->leaseLost(code);
        QCOMPARE(host.lease.calls, (QStringList{"start", "finish succeeded exited"}));
        QCOMPARE(host.session->phase(), Phase::Idle);
        QVERIFY(!host.clock.armed(SessionTimer::ResultRetention));
        host.deliver(JobResultAck{7, QStringLiteral("job-1")}); // too late: no second finish
        QCOMPARE(host.lease.calls.size(), 2);
    }
}

void RunnerSessionStopTest::finishReasons_data() {
    QTest::addColumn<qint64>("exitCode"); // -1: none
    QTest::addColumn<qint64>("signal");   // -1: none
    QTest::addColumn<QString>("finish");
    QTest::newRow("exit 0") << qint64(0) << qint64(-1) << "finish succeeded exited";
    QTest::newRow("exit 3") << qint64(3) << qint64(-1) << "finish failed exited_nonzero";
    QTest::newRow("signal") << qint64(-1) << qint64(11) << "finish failed signaled";
}

void RunnerSessionStopTest::finishReasons() {
    QFETCH(qint64, exitCode);
    QFETCH(qint64, signal);
    QFETCH(QString, finish);
    const auto given = [](const qint64 value) {
        return value < 0 ? std::nullopt : std::optional<qint64>(value);
    };
    Host host;
    host.launch(makeSpec());
    host.session->commandFinished(
        {QStringLiteral("exited"), given(exitCode), given(signal), {}, "verified"});
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    QCOMPARE(host.lease.calls, (QStringList{"start", finish}));
}

} // namespace runner_session_test
