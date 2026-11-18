#include "RunnerRunTest.h"

#include "run/RelayQueue.h"

#include <QJsonArray>
#include <QTest>

namespace runner_run_test {

using delivery::JournalRecord;
using delivery::LeaseCall;
using Readiness = CommandHost::Readiness;

namespace {

const QString draining = QStringLiteral("draining");
const QString idle = QStringLiteral("idle");
const QString busy = QStringLiteral("busy");

JournalRecord leasedRecord(const JournalRecord::Step step) {
    JournalRecord record;
    record.step = step;
    record.lease.jobId = QStringLiteral("job_9");
    record.lease.attempt = 1;
    record.lease.leaseId = QStringLiteral("lease_9");
    record.lease.leaseExpiresAt = QStringLiteral("2026-11-18T03:05:00.000Z");
    record.lease.machineId = QString::fromLatin1(vectors::machineId);
    record.lease.workspaceId = QString::fromLatin1(vectors::workspaceId);
    if (step == JournalRecord::Step::Started)
        record.startedAt = QStringLiteral("2026-11-18T02:59:00.000Z");
    return record;
}

// A listener for the queue alone.
class QueueListener final : public RelayQueueListener {
  public:
    QJsonObject heartbeatBody() override { return {}; }
    void heartbeatAnswered(const AccessTokens::Call&) override { answers.append("heartbeat"); }
    void leaseAnswered(const LeaseCall&, const delivery::CallResult&) override {
        answers.append("lease");
    }
    void tokenFailed(AccessTokens::Outcome) override {}
    void pairingsChecked(std::optional<ExitCode>) override { answers.append("pairings"); }
    QStringList answers;
};

} // namespace

// The command runner's leftovers are cleaned before the journal is restored; the acquire the
// journal names goes only after.
void RunnerRunStartTest::cleanupComesBeforeTheJournalAndAnyAcquire() {
    Run run;
    JournalRecord record;
    record.acquireId = QStringLiteral("acq_before");
    QVERIFY(run.journal->write(record));
    run.command.autoCleanup.reset();
    run.start();
    run.advance(60'000);
    QCOMPARE(run.loop->startup(), RunnerLoop::Startup::CleaningUp);
    QVERIFY(run.relay.leases.isEmpty());
    QCOMPARE(run.heartbeatState(), draining);

    run.command.finishCleanup(true);
    run.settle();
    QCOMPARE(run.loop->startup(), RunnerLoop::Startup::Running);
    QCOMPARE(run.relay.leases.size(), 1);
    QCOMPARE(run.relay.leases.first().kind, LeaseCall::Kind::Acquire);
    QCOMPARE(run.relay.leases.first().acquireId, QStringLiteral("acq_before"));
}

void RunnerRunStartTest::cleanupThatNeverAnswersIsBounded() {
    Run run;
    run.command.autoCleanup.reset();
    run.start();
    run.advance(startupCleanupBoundMs - 1);
    QCOMPARE(run.loop->startup(), RunnerLoop::Startup::CleaningUp);
    run.advance(1);
    QCOMPARE(run.loop->startup(), RunnerLoop::Startup::Running);
    QVERIFY(run.log().contains(QStringLiteral("did not end in 90 seconds")));
    // Unverified: draining, even with a ready command runner.
    QCOMPARE(run.loop->reportedState(), draining);
    run.command.finishCleanup(true); // a late answer still counts
    run.settle();
    QCOMPARE(run.loop->reportedState(), idle);
    run.advance(heartbeatSpacingMs);
    QCOMPARE(run.heartbeatState(), idle);
}

void RunnerRunStartTest::heartbeatReportsDrainingUntilStarted() {
    Run run;
    run.command.autoCleanup.reset();
    run.start();
    QCOMPARE(run.relay.heartbeats.size(), 1);
    const QJsonObject body = run.relay.heartbeats.first();
    QCOMPARE(body.keys(),
             (QStringList{"capabilities", "currentJob", "labels", "runnerVersion", "state"}));
    QCOMPARE(body.value("state").toString(), draining);
    QCOMPARE(body.value("labels").toArray(), QJsonArray{"gpu"});
    QCOMPARE(body.value("capabilities").toArray(), QJsonArray{"runner.protocol/1"});
    QVERIFY(body.value("currentJob").isNull());

    run.advance(500);
    run.command.finishCleanup(true);
    run.settle();
    // Two seconds after the last heartbeat, not 30.
    QCOMPARE(run.hub.remaining(runTimer(RunTimer::Heartbeat)), heartbeatSpacingMs - 500);
    run.advance(heartbeatSpacingMs - 500);
    QCOMPARE(run.relay.heartbeats.size(), 2);
    QCOMPARE(run.heartbeatState(), idle);
    QCOMPARE(run.hub.remaining(runTimer(RunTimer::Heartbeat)), heartbeatIntervalMs);
}

// A command runner that is ready from the start: idle, every 30 seconds.
void RunnerRunStartTest::readyRunnerReportsIdleEveryThirtySeconds() {
    Run run;
    run.start();
    run.advance(heartbeatIntervalMs * 3);
    QCOMPARE(run.relay.heartbeats.size(), 4);
    for (const QJsonObject& body : std::as_const(run.relay.heartbeats)) {
        QCOMPARE(body.value("state").toString(), idle);
        QVERIFY(body.value("currentJob").isNull());
    }
    QList<qint64> waits;
    for (const auto& [key, delay] : std::as_const(run.hub.arms))
        if (key.domain == TimerDomain::Run && key.id == static_cast<int>(RunTimer::Heartbeat))
            waits.append(delay);
    QCOMPARE(waits, (QList<qint64>{30000, 30000, 30000, 30000}));
}

void RunnerRunStartTest::acquireOnlyWhenEveryConditionHolds() {
    // The Relay socket is not ready.
    {
        Run run;
        run.start();
        run.acquire();
        QVERIFY(run.relay.leases.isEmpty());
        run.socketReady();
        run.acquire();
        QCOMPARE(run.leaseKinds().value(0), QStringLiteral("acquire"));
    }
    // The command runner is not ready, or not known yet.
    for (const Readiness readiness : {Readiness::Unavailable, Readiness::Unknown}) {
        Run run;
        run.command.state = readiness;
        run.start();
        run.socketReady();
        run.acquire();
        QVERIFY(run.relay.leases.isEmpty());
        QCOMPARE(run.heartbeatState(), draining);
    }
    // The start-up is not done.
    {
        Run run;
        run.command.autoCleanup.reset();
        run.start();
        run.socketReady();
        run.acquire();
        QVERIFY(run.relay.leases.isEmpty());
    }
    // The desktop disabled the runner.
    {
        Run run;
        run.start();
        run.socketReady();
        run.relay.heartbeatAnswer = [](const QJsonObject&) {
            AccessTokens::Call call;
            call.token.outcome = AccessTokens::Outcome::Ok;
            call.relay.kind = RelayResult::Kind::Ok;
            call.relay.body = {{"disabled", true}, {"pending", true}};
            return call;
        };
        run.advance(heartbeatIntervalMs);
        QVERIFY(run.relay.leases.isEmpty());
        QVERIFY(run.log().contains(QStringLiteral("disabled")));
    }
}

void RunnerRunStartTest::oneAcquirePerHeartbeatAnswer() {
    Run run;
    run.start();
    run.socketReady();
    run.relay.leaseAnswer = [](const LeaseCall&) { return refused("job_unavailable"); };
    run.relay.pending = true;
    run.advance(heartbeatIntervalMs * 3);
    const qsizetype pendingHeartbeats = run.relay.heartbeats.size() - 1; // the first said no
    QCOMPARE(count(run.leaseKinds(), "acquire"), static_cast<int>(pendingHeartbeats));
    QCOMPARE(pendingHeartbeats, 3);
}

void RunnerRunStartTest::jobAvailableAcquiresThatJob() {
    Run run;
    run.start();
    run.socketReady();
    run.wakeup({{"type", "job.available"}, {"jobId", "job_7"}});
    QCOMPARE(run.relay.leases.size(), 1);
    QCOMPARE(run.relay.leases.first().jobId, QStringLiteral("job_7"));
    // Holding a job: another wake-up takes nothing more.
    run.wakeup({{"type", "job.available"}, {"jobId", "job_8"}});
    QCOMPARE(count(run.leaseKinds(), "acquire"), 1);
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Held);
}

void RunnerRunStartTest::executorReadinessDrainsAndComesBack() {
    Run run;
    run.start();
    run.socketReady();
    run.command.set(Readiness::Unavailable);
    run.advance(heartbeatSpacingMs);
    QCOMPARE(run.heartbeatState(), draining);
    run.acquire();
    QVERIFY(run.relay.leases.isEmpty());

    run.command.set(Readiness::Ready);
    run.advance(heartbeatSpacingMs);
    QCOMPARE(run.heartbeatState(), idle);
    run.acquire();
    QCOMPARE(count(run.leaseKinds(), "acquire"), 1);
    QVERIFY(!run.exit);
}

void RunnerRunStartTest::readinessLostBeforeTheSpecReleasesTheLease() {
    Run run;
    run.start();
    run.socketReady();
    run.acquire();
    QCOMPARE(run.loop->session().phase(), session::JobSession::Phase::Assigned);
    run.command.set(Readiness::Unavailable);
    run.settle();
    QCOMPARE(run.leaseKinds(), (QStringList{"acquire", "release draining"}));
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
    QVERIFY(!run.journalRecord());
    QCOMPARE(run.loop->session().phase(), session::JobSession::Phase::Idle);
}

void RunnerRunStartTest::stateChangesSendAHeartbeatSoon() {
    Run run;
    run.start();
    run.socketReady();
    run.acquire(); // the heartbeat that said pending, then the lease
    const qsizetype before = run.relay.heartbeats.size();
    QCOMPARE(run.hub.remaining(runTimer(RunTimer::Heartbeat)), heartbeatSpacingMs);
    run.advance(heartbeatSpacingMs);
    QCOMPARE(run.relay.heartbeats.size(), before + 1);
    QCOMPARE(run.heartbeatState(), busy);
    const QJsonObject current = run.relay.heartbeats.last().value("currentJob").toObject();
    QCOMPARE(current.value("jobId").toString(), QStringLiteral("job_42"));
    QCOMPARE(current.value("leaseId").toString(), QStringLiteral("lease_1"));
    QCOMPARE(current.value("phase").toString(), QStringLiteral("assigned"));
    // Then the 30-second cadence again.
    QCOMPARE(run.hub.remaining(runTimer(RunTimer::Heartbeat)), heartbeatIntervalMs);
}

void RunnerRunStartTest::brokenJournalDrainsUntilRestart() {
    Run run;
    QString error;
    QVERIFY(run.state->write(QStringLiteral("journal.json"), "{broken", &error));
    run.start();
    run.socketReady();
    QCOMPARE(run.heartbeatState(), draining);
    run.acquire();
    QVERIFY(run.relay.leases.isEmpty());
    run.command.set(Readiness::Unavailable);
    run.command.set(Readiness::Ready);
    run.advance(heartbeatIntervalMs);
    QCOMPARE(run.heartbeatState(), draining);
    QCOMPARE(run.state->read(QStringLiteral("journal.json")).data, QByteArray("{broken"));
}

void RunnerRunStartTest::brokenAcceptedJobsDrainUntilRestart() {
    Run run;
    run.acceptedError = QStringLiteral("The record of accepted jobs cannot be read.");
    run.start();
    run.socketReady();
    QCOMPARE(run.heartbeatState(), draining);
    run.acquire();
    QVERIFY(run.relay.leases.isEmpty());
}

void RunnerRunStartTest::restoredLeaseWithoutSpecIsReleasedWhileDraining() {
    Run run;
    QVERIFY(run.journal->write(leasedRecord(JournalRecord::Step::Leased)));
    run.command.state = Readiness::Unavailable;
    run.start();
    QVERIFY(run.leaseKinds().contains(QStringLiteral("release draining")));
    QCOMPARE(run.loop->delivery().state(), delivery::JobDelivery::State::Idle);
    QVERIFY(!run.journalRecord());
}

// A job whose spec was taken never runs again: it ends interrupted, once, even when the clean-up
// is unverified, and the runner drains until the clean-up is confirmed.
void RunnerRunStartTest::restoredStartedJobFinishesInterrupted() {
    Run run;
    QVERIFY(run.journal->write(leasedRecord(JournalRecord::Step::Started)));
    run.command.autoCleanup = false;
    run.start();
    run.advance(heartbeatIntervalMs * 2);
    QCOMPARE(count(run.leaseKinds(), "finish interrupted runner_restarted"), 1);
    QVERIFY(!run.journalRecord());
    QCOMPARE(run.heartbeatState(), draining);
    QVERIFY(run.command.launches.isEmpty());
}

// Whatever happens while a Relay call blocks (timers, socket messages, readiness changes),
// no other call starts until it has returned.
void RunnerRunStartTest::relayCallsNeverNest() {
    Run run;
    run.relay.spinMs = 5;
    run.start();
    run.socketReady();
    int injected = 0;
    run.relay.during = [&](const QString&) {
        if (++injected > 12)
            return;
        run.hub.fire(runTimer(RunTimer::Heartbeat));
        run.loop->socketText(QStringLiteral(R"({"type":"job.available","jobId":"job_42"})"));
        run.command.set(Readiness::Ready);
        run.hub.fire(timerKey(delivery::DeliveryTimer::Renew));
    };
    run.relay.pending = true;
    run.advance(heartbeatIntervalMs);
    run.advance(delivery::renewIntervalMs);
    QCOMPARE(run.relay.maxDepth, 1);
    QCOMPARE(run.relay.depth, 0);
    QVERIFY(injected > 12);
    QCOMPARE(count(run.leaseKinds(), "acquire"), 1);
    QVERIFY(count(run.leaseKinds(), "renew") >= 1);
}

// The queue alone: asking for heartbeats while a call runs leaves one waiting, behind the renewal
// asked before it.
void RunnerRunStartTest::heartbeatsWaitAtMostOnce() {
    FakeRelayPort port;
    QueueListener listener;
    RelayQueue queue(port, listener);
    port.during = [&](const QString& name) {
        if (name != QStringLiteral("token"))
            return;
        LeaseCall renew;
        renew.kind = LeaseCall::Kind::Renew;
        renew.leaseId = QStringLiteral("lease_1");
        queue.send(renew);
        for (int i = 0; i < 5; ++i)
            queue.heartbeat();
        QVERIFY(queue.heartbeatWaiting());
    };
    queue.current([](const peer::TokenSource::Token&) {});
    QTRY_VERIFY(queue.idle());
    QCOMPARE(port.calls, (QStringList{"token", "renew", "heartbeat"}));
    QCOMPARE(listener.answers, (QStringList{"lease", "heartbeat"}));
    QCOMPARE(port.maxDepth, 1);
}

// job.cancel renews at once; the renewal timer is not doubled after it.
void RunnerRunStartTest::cancelWakeupRenewsOnce() {
    Run run;
    run.start();
    run.socketReady();
    run.acquire();
    run.connectDesktop();
    run.deliver(makeSpec());
    run.receiver.ready(QStringLiteral("/work/space"), {});
    run.settle();
    run.advance(10'000);
    QCOMPARE(count(run.leaseKinds(), "renew"), 0);
    run.wakeup({{"type", "job.cancel"}, {"jobId", "job_42"}, {"leaseId", "lease_1"}});
    QCOMPARE(count(run.leaseKinds(), "renew"), 1);
    // Another lease's cancel changes nothing.
    run.wakeup({{"type", "job.cancel"}, {"jobId", "job_42"}, {"leaseId", "lease_other"}});
    QCOMPARE(count(run.leaseKinds(), "renew"), 1);
    QCOMPARE(run.hub.remaining(timerKey(delivery::DeliveryTimer::Renew)),
             delivery::renewIntervalMs);
    run.advance(delivery::renewIntervalMs - 1);
    QCOMPARE(count(run.leaseKinds(), "renew"), 1);
    run.advance(1);
    QCOMPARE(count(run.leaseKinds(), "renew"), 2);
}

void RunnerRunStartTest::logsHoldNoSecret() {
    QStringList messages;
    static QStringList* sink = nullptr;
    sink = &messages;
    const QtMessageHandler previous =
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
            sink->append(message);
        });
    {
        Run run;
        run.start();
        run.socketReady();
        run.acquire();
        run.connectDesktop();
        run.deliver(makeSpec());
        run.receiver.ready(QStringLiteral("/work/space"), {});
        run.settle();
        QCOMPARE(run.command.launches.size(), 1);
        run.command.events.output(QByteArray(commandMarker));
        run.command.events.finished({});
        run.settle();
        messages += run.lines;
    }
    qInstallMessageHandler(previous);
    sink = nullptr;
    const QString all = messages.join(QLatin1Char('\n'));
    QVERIFY(!all.isEmpty());
    for (const QString& secret :
         {QString::fromLatin1(tokenMarker), QString::fromLatin1(commandMarker),
          QStringLiteral("v=0"), QStringLiteral("a=fingerprint"), QStringLiteral("/work/space"),
          QStringLiteral("/usr/bin:/bin")})
        QVERIFY2(!all.contains(secret), qPrintable(secret));
}

} // namespace runner_run_test
