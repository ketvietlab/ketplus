#include "RunnerSessionTest.h"

#include "protocol/FrameLimits.h"

#include <QTest>

namespace runner_session_test {

using namespace protocol;
using Phase = session::JobSession::Phase;

namespace {

enum class Setup {
    OtherJob,      // a spec for a job this runner does not hold
    InvalidField,  // the leased job, a field out of its limits
    NoContainment, // the admission probe fails
    StartRefused,  // the Relay refuses `job.start`
    Withdrawn,     // the Relay answered `unsupported` before the spec
    StartUnsupported,
};

} // namespace

void RunnerSessionSpecTest::refusals_data() {
    QTest::addColumn<int>("setup");
    QTest::addColumn<QString>("code");
    QTest::addColumn<QStringList>("lease");
    QTest::newRow("job_mismatch") << int(Setup::OtherJob) << "job_mismatch" << QStringList{};
    QTest::newRow("spec_invalid") << int(Setup::InvalidField) << "spec_invalid"
                                  << QStringList{"finish rejected spec_invalid"};
    QTest::newRow("containment_unavailable")
        << int(Setup::NoContainment) << "containment_unavailable"
        << QStringList{"finish rejected containment_unavailable"};
    QTest::newRow("lease_lost") << int(Setup::StartRefused) << "lease_lost" << QStringList{"start"};
    QTest::newRow("runner_unavailable before start")
        << int(Setup::Withdrawn) << "runner_unavailable"
        << QStringList{"finish rejected runner_unavailable"};
    QTest::newRow("runner_unavailable from start")
        << int(Setup::StartUnsupported) << "runner_unavailable"
        << QStringList{"start", "finish rejected runner_unavailable"};
}

void RunnerSessionSpecTest::refusals() {
    QFETCH(int, setup);
    QFETCH(QString, code);
    QFETCH(QStringList, lease);
    Host host;
    host.connect();
    host.clearSent();
    JobSpec spec = makeSpec();
    switch (Setup(setup)) {
    case Setup::OtherJob:
        host.deliver(makeSpec(QStringLiteral("job-2")));
        break;
    case Setup::InvalidField: {
        const QByteArray message =
            R"({"type":"job.spec","epoch":7,"jobId":"job-1","attempt":1,"timeoutMs":5})";
        host.deliverRaw(Channel::Control, message);
        break;
    }
    case Setup::NoContainment:
        host.command.admits = false;
        host.deliver(spec);
        break;
    case Setup::StartRefused:
        host.deliver(spec);
        host.session->leaseStartRefused(QStringLiteral("lease_expired"));
        break;
    case Setup::Withdrawn:
        host.session->leaseLost(QStringLiteral("unsupported"));
        host.deliver(spec);
        break;
    case Setup::StartUnsupported:
        host.deliver(spec);
        host.session->leaseStartRefused(QStringLiteral("unsupported"));
        break;
    }
    QCOMPARE(host.sentTypes(), QStringList{"job.rejected"});
    QCOMPARE(host.sentOf<JobRejected>().first().code, code);
    QCOMPARE(host.lease.calls, lease);
    QVERIFY(host.snapshots.begun.isEmpty());
    QVERIFY(host.command.launches.isEmpty()); // a refused spec never reaches the command
    QVERIFY(host.channel.closes.isEmpty());   // and the connection stays
    if (Setup(setup) == Setup::OtherJob)
        QCOMPARE(host.session->phase(), Phase::Assigned); // still waiting for its own spec
    else
        QCOMPARE(host.session->phase(), Phase::Idle);
}

void RunnerSessionSpecTest::orderOfStartAcceptedAndStarted() {
    Host host;
    host.connect();
    host.clearSent();
    const JobSpec spec = makeSpec();
    host.deliver(spec);
    QCOMPARE(host.lease.calls, QStringList{"start"});
    QVERIFY(host.channel.sent.isEmpty()); // no `job.accepted` before the Relay started the job
    QCOMPARE(host.session->phase(), Phase::Starting);
    host.session->leaseStarted(startedAt);
    QCOMPARE(host.sentTypes(), QStringList{"job.accepted"});
    QCOMPARE(host.snapshots.begun, QStringList{"job-1"});
    // The receiver's own frames go out while it syncs.
    host.snapshots.send(SnapshotWant{spec.snapshotId, QStringLiteral("need"), 0});
    host.deliver(SnapshotOffer{7,
                               spec.jobId,
                               spec.snapshotId,
                               std::nullopt,
                               100,
                               QString(64, QLatin1Char('d')),
                               limits::chunkBytes,
                               {}});
    QCOMPARE(host.snapshots.received.size(), 1);
    QVERIFY(host.command.launches.isEmpty()); // nothing runs before the snapshot is ready
    host.session->snapshotReady(QStringLiteral("/work/ws"), {QStringLiteral("lfs_not_synced")});
    QCOMPARE(host.command.launches.size(), 1);
    QCOMPARE(host.sentTypes(),
             (QStringList{"job.accepted", "snapshot.want", "snapshot.ready", "job.started"}));
    QCOMPARE(host.sentOf<JobStarted>().first().startedAt, startedAt);
    QCOMPARE(host.session->phase(), Phase::Running);
    host.session->commandFinished({QStringLiteral("exited"), 0, std::nullopt, {}, "verified"});
    const JobResult result = host.sentOf<JobResult>().first();
    QCOMPARE(result.warnings, QStringList{"lfs_not_synced"});
    QCOMPARE(host.lease.calls, QStringList{"start"}); // finish waits for the acknowledgement
    host.deliver(JobResultAck{7, spec.jobId});
    QCOMPARE(host.lease.calls, (QStringList{"start", "finish succeeded exited"}));
    QCOMPARE(host.session->phase(), Phase::Idle);
}

void RunnerSessionSpecTest::environmentOfTheCommand() {
    Host host;
    host.launch(makeSpec());
    const session::LaunchRequest request = host.command.launches.first();
    QCOMPARE(request.workspace, QStringLiteral("/work/ws"));
    QCOMPARE(request.cwd, QStringLiteral("src"));
    QCOMPARE(*request.invocation.argv, (QStringList{"make", "test"}));
    const QMap<QString, QString> expected{{"PATH", "/usr/bin"},
                                          {"HOME", "/home/runner"}, // LANG is unset by the spec
                                          {"CC", "clang"},
                                          {"KETPLUS_SNAPSHOT_ID", QString(40, QLatin1Char('a'))},
                                          {"KETPLUS_SOURCE_HEAD", QString(40, QLatin1Char('b'))}};
    QCOMPARE(request.environment, expected);
}

void RunnerSessionSpecTest::repeatedSpecNeverLaunchesTwice() {
    Host host;
    const JobSpec spec = makeSpec();
    host.connect();
    host.deliver(spec);
    host.deliver(spec); // while `job.start` is asked: answered by the start's outcome
    QCOMPARE(host.lease.calls, QStringList{"start"});
    host.session->leaseStarted(startedAt);
    host.clearSent();
    host.deliver(spec); // syncing
    QCOMPARE(host.sentTypes(), QStringList{"job.accepted"});
    host.session->snapshotReady(QStringLiteral("/work/ws"), {});
    host.clearSent();
    host.deliver(spec); // running
    QCOMPARE(host.sentTypes(), (QStringList{"job.accepted", "job.started"}));
    // A new connection, the same spec again.
    host.session->channelLost();
    host.open(QStringLiteral("c2"));
    host.welcome();
    host.clearSent();
    host.deliver(spec);
    QCOMPARE(host.sentTypes(), (QStringList{"job.accepted", "job.started"}));
    host.session->commandFinished({QStringLiteral("exited"), 3, std::nullopt, {}, "verified"});
    host.deliver(spec); // with a result
    QVERIFY(host.sentTypes().endsWith("job.result"));
    QCOMPARE(host.command.launches.size(), 1);
    QCOMPARE(host.lease.calls, QStringList{"start"});
    QCOMPARE(host.snapshots.begun.size(), 1);
}

void RunnerSessionSpecTest::otherJobWhileBusy() {
    Host host;
    host.launch(makeSpec());
    host.clearSent();
    host.deliver(makeSpec(QStringLiteral("job-2")));
    QCOMPARE(host.sentOf<JobRejected>().first().code, QStringLiteral("runner_busy"));
    host.deliver(makeSpec(QStringLiteral("job-1"), 2)); // another attempt is another job
    QCOMPARE(host.sentOf<JobRejected>().last().code, QStringLiteral("runner_busy"));
    QCOMPARE(host.command.launches.size(), 1);
    QCOMPARE(host.session->phase(), Phase::Running);
}

void RunnerSessionSpecTest::snapshotFailureEndsTheJob() {
    Host host;
    host.accept(makeSpec());
    host.clearSent();
    host.session->snapshotFailed(QStringLiteral("disk_full"));
    QCOMPARE(host.sentTypes(), (QStringList{"snapshot.error", "job.result"}));
    const JobResult result = host.sentOf<JobResult>().first();
    QCOMPARE(result.state, QStringLiteral("failed"));
    QCOMPARE(result.reason, QStringLiteral("disk_full"));
    QCOMPARE(result.cleanupStatus, QStringLiteral("verified"));
    QVERIFY(!result.startedAt);
    QVERIFY(host.command.launches.isEmpty());
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    QCOMPARE(host.lease.calls, (QStringList{"start", "finish failed disk_full"}));
}

void RunnerSessionSpecTest::nextJobAfterTheResult() {
    Host host;
    host.launch(makeSpec());
    host.session->commandFinished({QStringLiteral("exited"), 0, std::nullopt, {}, "verified"});
    host.deliver(JobResultAck{7, QStringLiteral("job-1")});
    QCOMPARE(host.session->phase(), Phase::Idle);
    QVERIFY(host.clock.deadlines.isEmpty());
    // The next lease gets a new connection and a clean output.
    QVERIFY(host.session->assign(assignment(QStringLiteral("job-2"))));
    QCOMPARE(host.channel.closes, QStringList{"job_finished"});
    QCOMPARE(host.channel.connectionRequests, 2);
    host.open(QStringLiteral("c2"));
    host.welcome();
    host.deliver(makeSpec(QStringLiteral("job-2")));
    host.session->leaseStarted(startedAt);
    host.session->snapshotReady(QStringLiteral("/work/ws"), {});
    QCOMPARE(host.command.launches.size(), 2);
    QCOMPARE(host.session->output().end(), 0);
    // The finished job's spec is never taken again.
    host.clearSent();
    host.deliver(makeSpec());
    QCOMPARE(host.sentOf<JobRejected>().first().code, QStringLiteral("runner_busy"));
}

} // namespace runner_session_test
