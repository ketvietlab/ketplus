// Stops, timers and failures of a remote job through RemoteJobs with real parts: the Relay and
// the runner are played by the test, time moves only when the test moves it.
#include "RemoteJobsRunnerEnd.h"
#include "storage/BackgroundTaskOutput.h"

#include <QTest>

#include <algorithm>

using namespace remote_jobs_test;

namespace {

QStringList capturedLog;
void captureLog(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedLog.append(message);
}

// Every timer armed for `taskId`, of any kind.
int armedFor(const remote_job_test::FakeClock& clock, const QString& taskId) {
    int armed = 0;
    for (const auto& [key, due] : clock.due)
        armed += key.first == taskId ? 1 : 0;
    return armed;
}

int outboxOf(LedgerFixture& f, const QString& taskId) {
    return f.count("SELECT count(*) FROM managed_job_outbox WHERE task_id='" + taskId + "'");
}

template <typename T> QList<T> framesOf(const QList<protocol::Frame>& frames) {
    QList<T> out;
    for (const protocol::Frame& frame : frames)
        if (const T* found = std::get_if<T>(&frame))
            out.append(*found);
    return out;
}

protocol::JobResult resultOf(const QString& state, const std::optional<qint64> exitCode = {}) {
    protocol::JobResult result;
    result.jobId = linkedJob;
    result.state = state;
    result.exitCode = exitCode;
    result.cleanupStatus = QStringLiteral("verified");
    return result;
}

// A job whose command runs on the runner: the snapshot is in and `job.started` sent.
struct Running {
    EndToEnd e;
    QString id;
    std::unique_ptr<RunnerEnd> end;

    bool start(const qint64 timeoutMs = 60'000) {
        id = e.submit(timeoutMs);
        if (id.isEmpty())
            return false;
        e.h.clock.target = &e.h.jobs->coordinator();
        e.assign();
        end = std::make_unique<RunnerEnd>(e.connect(1));
        if (!e.play(*end, [&] { return e.ready; }, false))
            return false;
        end->send(protocol::JobStarted{linkedJob, utc(e.h.clock.time)});
        e.started = true;
        return e.h.jobs->record(id)->state == state::running;
    }
    // A new RemoteJobs on the same registry, as after a daemon restart.
    void restart() {
        e.h.build();
        e.h.clock.due.clear(); // the timers of the stopped daemon end with it
        e.h.clock.target = &e.h.jobs->coordinator();
        e.h.jobs->restore();
    }
};

} // namespace

class RemoteJobsStopTest final : public QObject {
    Q_OBJECT

  private slots:
    void aStopBeforeAnyRunnerCancelsOnTheRelay() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        QVERIFY(e.h.jobs->stop(id).ok());
        QCOMPARE(e.h.jobs->record(id)->state, state::stopping);
        QCOMPARE(e.h.relay.pending(QStringLiteral("cancel")), 1);
        remote_job_test::RelayJob cancelled =
            remote_job_test::relayJob(QStringLiteral("cancelled"));
        cancelled.jobId = linkedJob;
        QVERIFY(e.h.relay.answer(QStringLiteral("cancel"), cancelled));
        QCOMPARE(e.h.jobs->record(id)->state, state::cancelled);
        QCOMPARE(e.h.f.state(id), state::cancelled);
        QCOMPARE(outboxOf(e.h.f, id), 1);
        QVERIFY(e.parts.transports.wires.isEmpty());
    }

    void aStopWhileRunningCancelsOnTheRunner() {
        Running r;
        QVERIFY(r.start());
        QVERIFY(r.e.h.jobs->stop(r.id).ok());
        QVERIFY(
            r.e.play(*r.end, [&] { return !framesOf<protocol::JobCancel>(r.e.seen).isEmpty(); }));
        QCOMPARE(framesOf<protocol::JobCancel>(r.e.seen).first().reason, QStringLiteral("user"));
        r.end->send(resultOf(QStringLiteral("cancelled")));
        QVERIFY(r.e.play(*r.end, [&] { return r.e.resultAcked; }));
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::cancelled);
        QCOMPARE(r.e.h.f.state(r.id), state::cancelled);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // The runner holds the timeout of its spec: the desktop's deadline only marks the record
    // and sends no cancel; the runner's result ends it.
    void theDeadlineWaitsForTheRunnersResult() {
        Running r;
        QVERIFY(r.start());
        r.e.h.clock.advance(60'000);
        const auto stopping = r.e.h.jobs->record(r.id);
        QCOMPARE(stopping->state, state::stopping);
        QCOMPARE(stopping->stopReason, QStringLiteral("deadline"));
        r.end->send(resultOf(QStringLiteral("timed_out")));
        QVERIFY(r.e.play(*r.end, [&] { return r.e.resultAcked; }));
        QVERIFY(framesOf<protocol::JobCancel>(r.e.seen).isEmpty());
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::timedOut);
        QCOMPARE(r.e.h.f.state(r.id), state::timedOut);
    }

    void twentyMinutesWithoutTheRunnerEndTheJob() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        r.end->lose();
        r.e.h.clock.advance(20 * 60 * 1000 - 1);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);
        r.e.h.clock.advance(1);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::interrupted);
        QCOMPARE(r.e.h.jobs->record(r.id)->cleanupStatus, cleanup::unverified);
        QCOMPARE(r.e.h.f.state(r.id), state::interrupted);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // A restart while the command runs: the record goes on, its 20-minute window counts from
    // the restart, and the runner that comes back finishes the job.
    void aRestartWhileRunningCountsTheWindowAgain() {
        constexpr qint64 minute = 60 * 1000;
        Running r;
        QVERIFY(r.start(2 * 60 * minute));
        r.end->lose();
        r.e.h.clock.advance(15 * minute);
        r.e.h.build();
        r.e.h.clock.due.clear(); // the timers of the stopped daemon end with it
        r.e.h.clock.target = &r.e.h.jobs->coordinator();
        r.e.h.jobs->restore();
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);
        r.e.h.clock.advance(19 * minute);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);

        r.e.seen.clear();
        r.e.seenTypes.clear();
        RunnerEnd second = r.e.connect(2);
        QVERIFY(
            r.e.play(second, [&] { return !framesOf<protocol::JobResume>(r.e.seen).isEmpty(); }));
        QVERIFY(framesOf<protocol::JobSpec>(r.e.seen).isEmpty());
        protocol::JobResult result = resultOf(QStringLiteral("exited"), 0);
        second.send(result);
        QVERIFY(r.e.play(second, [&] { return r.e.resultAcked; }));
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::completed);
        QCOMPARE(r.e.h.f.state(r.id), state::completed);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // A record ended for want of its runner stays ended through a restart: no window and no
    // Relay call.
    void aRecordEndedWithoutItsRunnerStaysEndedAcrossARestart() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        r.end->lose();
        r.e.h.clock.advance(20 * 60 * 1000);
        const auto before = r.e.h.jobs->record(r.id);
        QCOMPARE(before->state, state::interrupted);
        const int relayCalls = int(r.e.h.relay.log.size());

        r.restart();
        QCOMPARE(r.e.h.jobs->record(r.id), before);
        QCOMPARE(r.e.h.f.state(r.id), state::interrupted);
        QCOMPARE(armedFor(r.e.h.clock, r.id), 0);
        QCOMPARE(int(r.e.h.relay.log.size()), relayCalls);
        r.e.h.clock.advance(60 * 60 * 1000);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::interrupted);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // A stop the runner accepted goes on after a restart: the window counts again, and the
    // runner that comes back is resumed and told to cancel.
    void aStopTheRunnerAcceptedGoesOnAfterARestart() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        QVERIFY(r.e.h.jobs->stop(r.id).ok());
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::stopping);
        r.end->lose();

        r.restart();
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::stopping);
        QVERIFY(r.e.h.clock.isArmed(r.id, JobTimer::PeerAbsent));
        QCOMPARE(outboxOf(r.e.h.f, r.id), 0);

        r.e.seen.clear();
        r.e.seenTypes.clear();
        RunnerEnd second = r.e.connect(2);
        QVERIFY(r.e.play(
            second, [&] { return !framesOf<protocol::JobCancel>(r.e.seen).isEmpty(); }, false));
        QCOMPARE(r.e.seenTypes.mid(0, 2), (QStringList{"welcome", "job.resume"}));
        QCOMPARE(framesOf<protocol::JobCancel>(r.e.seen).first().reason, QStringLiteral("user"));
        second.send(resultOf(QStringLiteral("cancelled")));
        QVERIFY(r.e.play(second, [&] { return r.e.resultAcked; }));
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::cancelled);
        QCOMPARE(r.e.h.f.state(r.id), state::cancelled);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // Another account: the links of the old one end with their connections, which the job
    // counts as lost; the runner comes back on the new links and the job goes on.
    void anAccountChangeEndsTheOldLinks() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        r.e.h.scope = QStringLiteral("account-2");
        QVERIFY(r.e.h.jobs->relayMessage({{QStringLiteral("type"), QStringLiteral("job.changed")},
                                          {QStringLiteral("jobId"), linkedJob}}));
        QVERIFY(r.end->closed());
        QVERIFY(r.e.h.jobs->linksOpen());
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);
        QVERIFY(r.e.h.clock.isArmed(r.id, JobTimer::PeerAbsent));

        r.e.seen.clear();
        r.e.seenTypes.clear();
        RunnerEnd second = r.e.connect(2);
        QVERIFY(
            r.e.play(second, [&] { return !framesOf<protocol::JobResume>(r.e.seen).isEmpty(); }));
        QVERIFY(!r.e.h.clock.isArmed(r.id, JobTimer::PeerAbsent));
    }

    void aRemovedRunnerEndsItsRunningJob() {
        Running r;
        QVERIFY(r.start());
        QVERIFY(r.e.h.jobs->runnerRemoved(linkedRunner, QStringLiteral("client-1")).ok());
        const auto record = r.e.h.jobs->record(r.id);
        QVERIFY(record);
        QCOMPARE(record->state, state::interrupted);
        QCOMPARE(record->reason, QStringLiteral("runner_removed"));
        QCOMPARE(record->cleanupStatus, cleanup::unverified);
        QCOMPARE(r.e.h.f.state(r.id), state::interrupted);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
        QVERIFY(r.e.play(*r.end, [&] { return r.end->closed(); }));
    }

    void anOfferIsRefusedWhenTheSpoolCannotOpen() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        e.assign();
        e.parts.failSpool = true;
        e.h.jobs->relayMessage(signedOffer(linkConnection(1), linkedJob, 1, e.parts.clock.time));
        QVERIFY(e.parts.transports.wires.isEmpty());
        QCOMPARE(e.parts.out.count(QStringLiteral("signal.answer")), 0);
        QCOMPARE(e.parts.out.count(QStringLiteral("signal.end")), 1);
        QCOMPARE(e.parts.out.sent.last().value("reason").toString(),
                 QStringLiteral("storage_error"));
        QVERIFY(!e.h.jobs->record(id)->terminal());
    }

    // The Relay socket opened again: the records not yet dispatched are asked once.
    void aReopenedSocketAsksTheRelayOnce() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        const int before = e.h.relay.sent(QStringLiteral("get"));
        e.h.jobs->relaySocketReopened();
        QCOMPARE(e.h.relay.sent(QStringLiteral("get")), before + 1);
    }

    // The Relay stops offering runners (or the daemon signs out) while a job runs: new submits
    // are refused, the job goes on over its peer, its output is kept and its result ends it.
    void aRunningJobOutlivesTheRelaysRunnerOffer() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        r.e.h.relay.offered = false;
        QVERIFY(!r.e.h.jobs->available());
        QString refused;
        r.e.h.jobs->submit(r.e.h.f.remoteTask(), {}, [&](const QString& code) { refused = code; });
        QCOMPARE(refused, QStringLiteral("runner_unavailable"));

        r.end->send(protocol::JobOutput{linkedJob, 0, QByteArrayLiteral("abc")});
        protocol::JobResult result = resultOf(QStringLiteral("exited"), 0);
        result.outputBytes = 3;
        r.end->send(result);
        QVERIFY(r.e.play(*r.end, [&] { return r.e.resultAcked; }));
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::completed);
        QCOMPARE(r.e.h.f.state(r.id), state::completed);
        QCOMPARE(ketplus::daemon::BackgroundTaskOutput(r.e.h.f.database).read(r.id).bytes,
                 QByteArrayLiteral("abc"));
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // The Relay socket drops and opens again while a job runs: the job goes on over its peer,
    // the Relay is asked once about the record still waiting there, and the running job ends
    // with one completion only, also after another reopening.
    void aRunningJobOutlivesADroppedRelaySocket() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        const QString waiting = r.e.submit(60'000, QStringLiteral("job-2"));
        QVERIFY(!waiting.isEmpty());
        const int before = r.e.h.relay.sent(QStringLiteral("get"));
        r.e.h.jobs->relaySocketReopened();
        QCOMPARE(r.e.h.relay.sent(QStringLiteral("get")), before + 1);
        QVERIFY(!r.end->closed());
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);
        QVERIFY(!r.e.h.clock.isArmed(r.id, JobTimer::PeerAbsent));

        r.end->send(resultOf(QStringLiteral("exited"), 0));
        QVERIFY(r.e.play(*r.end, [&] { return r.e.resultAcked; }));
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::completed);
        r.e.h.jobs->relaySocketReopened();
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::completed);
        QCOMPARE(r.e.h.f.state(r.id), state::completed);
        QCOMPARE(outboxOf(r.e.h.f, r.id), 1);
    }

    // Both the Relay and the runner's peer gone for 20 minutes: the record ends.
    void aRunningJobWithoutRelayOrPeerEnds() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        r.e.h.relay.offered = false;
        r.end->lose();
        r.e.h.clock.advance(20 * 60 * 1000 - 1);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);
        r.e.h.clock.advance(1);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::interrupted);
        QCOMPARE(r.e.h.f.state(r.id), state::interrupted);
    }
};

QTEST_GUILESS_MAIN(RemoteJobsStopTest)
#include "RemoteJobsStopTest.moc"
