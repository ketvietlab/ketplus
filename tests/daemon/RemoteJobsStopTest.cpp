// Stops, timers and failures of a remote job through RemoteJobs with real parts: the Relay and
// the runner are played by the test, time moves only when the test moves it.
#include "RemoteJobsRunnerEnd.h"

#include <QTest>

using namespace remote_jobs_test;

namespace {

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

    void twentyMinutesWithoutTheRunnerNeedAttention() {
        Running r;
        QVERIFY(r.start(2 * 60 * 60 * 1000));
        r.end->lose();
        r.e.h.clock.advance(20 * 60 * 1000 - 1);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::running);
        r.e.h.clock.advance(1);
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::needsAttention);
        QCOMPARE(r.e.h.f.state(r.id), state::needsAttention);

        QVERIFY(!r.e.h.jobs->resolveCleanup(r.id, {}).ok());
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::needsAttention);
        QVERIFY(r.e.h.jobs->resolveCleanup(r.id, QStringLiteral("client-1")).ok());
        QCOMPARE(r.e.h.jobs->record(r.id)->state, state::interrupted);
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

    void aRemovedRunnerEndsItsRunningJob() {
        Running r;
        QVERIFY(r.start());
        QVERIFY(r.e.h.jobs->runnerRemoved(linkedRunner, QStringLiteral("client-1")).ok());
        const auto record = r.e.h.jobs->record(r.id);
        QVERIFY(record);
        QCOMPARE(record->state, state::interrupted);
        QCOMPARE(record->reason, QStringLiteral("cleanup_acknowledged"));
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
};

QTEST_GUILESS_MAIN(RemoteJobsStopTest)
#include "RemoteJobsStopTest.moc"
