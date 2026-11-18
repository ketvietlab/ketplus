#include "RunnerRunTest.h"

#include <QTest>

namespace runner_run_test {

using delivery::JournalRecord;
using delivery::LeaseCall;

namespace {

void runJob(Run& run) {
    run.acquire();
    run.connectDesktop();
    run.deliver(makeSpec());
    run.receiver.ready(QStringLiteral("/work/space"), {});
    run.settle();
}

} // namespace

void RunnerRunStopTest::stopWhileIdleExitsZero() {
    Run run;
    run.start();
    run.socketReady();
    run.stops = 1;
    run.advance(stopPollMs);
    QCOMPARE(run.exit, std::optional(ExitCode::Ok));
    QVERIFY(!run.socket->live);
    QVERIFY(run.relay.leases.isEmpty());
    QVERIFY(run.log().contains(QStringLiteral("Stopped.")));
}

// A stop that comes while an acquire is on the wire gives back the lease it brings.
void RunnerRunStopTest::stopWaitsForAnAcquireInFlight() {
    Run run;
    run.start();
    run.socketReady();
    run.relay.during = [&run](const QString& name) {
        if (name == QStringLiteral("acquire"))
            run.stops = 1;
    };
    run.acquire(); // the stop is seen at the next poll, while the heartbeats go on
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "release draining"}));
    QCOMPARE(run.exit, std::optional(ExitCode::Ok));
    QVERIFY(!run.journalRecord());
}

void RunnerRunStopTest::stopWithAJobWaitsForItsFinish() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    run.stops = 1;
    run.advance(heartbeatSpacingMs);
    QVERIFY(!run.exit);
    QCOMPARE(run.heartbeatState(), QStringLiteral("draining"));
    QVERIFY(run.relay.heartbeats.last().value("currentJob").isObject());
    QCOMPARE(run.command.kills, 0);

    session::CommandExit exit;
    exit.exitCode = 0;
    exit.cleanupStatus = QStringLiteral("verified");
    run.command.events.finished(exit);
    run.settle();
    QVERIFY(!run.exit);
    run.deliver(protocol::JobResultAck{7, QStringLiteral("job_42")});
    run.advance(stopPollMs);
    QCOMPARE(run.leaseKinds().last(), QStringLiteral("finish succeeded exited"));
    QCOMPARE(run.exit, std::optional(ExitCode::Ok));
    QCOMPARE(count(run.leaseKinds(), "acquire"), 1);
}

void RunnerRunStopTest::secondSignalExitsAtOnce() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    const auto before = run.journalRecord();
    QVERIFY(before);
    run.stops = 2;
    run.advance(stopPollMs);
    QCOMPARE(run.exit, std::optional(ExitCode::Failure));
    QCOMPARE(run.command.kills, 0);
    QCOMPARE(run.journalRecord()->step, before->step);
    QCOMPARE(run.journalRecord()->lease.leaseId, before->lease.leaseId);
    QVERIFY(!run.leaseKinds().join(',').contains(QStringLiteral("finish")));
}

// The job the second signal left behind is reported interrupted at the next start.
void RunnerRunStopTest::restartAfterASecondSignal() {
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    run.stops = 2;
    run.advance(stopPollMs);
    QVERIFY(run.exit);

    run.loop.reset();
    run.exit.reset();
    run.stops = 0;
    run.relay.leases.clear();
    run.start();
    QCOMPARE(run.leaseKinds().value(0), QStringLiteral("finish interrupted runner_restarted"));
    QCOMPARE(count(run.leaseKinds(), "finish interrupted runner_restarted"), 1);
    QCOMPARE(count(run.leaseKinds(), "acquire"), 0);
    QVERIFY(!run.journalRecord());
    QVERIFY(!run.exit);
}

void RunnerRunStopTest::fatalRefusalsExitAtOnce_data() {
    QTest::addColumn<QString>("where");
    QTest::addColumn<int>("code");
    QTest::newRow("heartbeat runner_removed") << "heartbeat" << toInt(ExitCode::RunnerRemoved);
    QTest::newRow("heartbeat credential refused") << "token" << toInt(ExitCode::InvalidAccess);
    QTest::newRow("renew runner_removed") << "renew" << toInt(ExitCode::RunnerRemoved);
    QTest::newRow("renew credential lost") << "renew lost" << toInt(ExitCode::InvalidAccess);
    QTest::newRow("socket runner removed") << "socket" << toInt(ExitCode::RunnerRemoved);
}

// During a job, a removed runner or a refused credential ends the process at once.
void RunnerRunStopTest::fatalRefusalsExitAtOnce() {
    QFETCH(QString, where);
    QFETCH(int, code);
    Run run;
    run.start();
    run.socketReady();
    runJob(run);
    if (where == QStringLiteral("heartbeat")) {
        run.relay.heartbeatAnswer = [](const QJsonObject&) {
            AccessTokens::Call call;
            call.token.outcome = AccessTokens::Outcome::Ok;
            call.relay.kind = RelayResult::Kind::Refused;
            call.relay.status = 404;
            call.relay.code = QStringLiteral("runner_removed");
            return call;
        };
        run.advance(heartbeatIntervalMs);
    } else if (where == QStringLiteral("token")) {
        run.relay.heartbeatAnswer = [](const QJsonObject&) {
            AccessTokens::Call call;
            call.token.outcome = AccessTokens::Outcome::InvalidAccess;
            return call;
        };
        run.advance(heartbeatIntervalMs);
    } else if (where.startsWith(QStringLiteral("renew"))) {
        const bool lost = where.endsWith(QStringLiteral("lost"));
        run.relay.leaseAnswer = [lost](const LeaseCall&) {
            if (!lost)
                return refused(QStringLiteral("runner_removed"));
            delivery::CallResult result;
            result.kind = delivery::CallResult::Kind::CredentialLost;
            return result;
        };
        run.advance(delivery::renewIntervalMs);
    } else {
        run.loop->socketClosed(4003);
        run.settle();
    }
    QCOMPARE(run.exit.has_value(), true);
    QCOMPARE(toInt(*run.exit), code);
    QCOMPARE(run.command.kills, 0);
    QVERIFY(run.journalRecord());
}

// The process ends from inside a blocking Relay call; the call returns into objects that are
// all still there, and nothing more is asked of the Relay.
void RunnerRunStopTest::exitInsideANestedCallDestroysNothing() {
    Run run;
    run.relay.spinMs = 5;
    run.start();
    run.socketReady();
    runJob(run);
    run.relay.during = [&run](const QString& name) {
        if (name == QStringLiteral("renew"))
            run.loop->socketClosed(4003);
    };
    const qsizetype before = run.relay.calls.size();
    run.advance(delivery::renewIntervalMs);
    QCOMPARE(run.exit, std::optional(ExitCode::RunnerRemoved));
    QCOMPARE(run.relay.depth, 0);
    QVERIFY(run.relay.calls.size() > before);
    QCOMPARE(run.relay.calls.last(), QStringLiteral("renew"));
    QVERIFY(run.transports.made.last()->alive);
    const qsizetype after = run.relay.calls.size();
    run.advance(heartbeatIntervalMs * 2);
    QCOMPARE(run.relay.calls.size(), after);
}

} // namespace runner_run_test
