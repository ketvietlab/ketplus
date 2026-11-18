// One remote job through RemoteJobs with every desktop part real: the registry in SQLite, the
// coordinator, the peer sessions, the runner links, the snapshot sender on a real repository and
// the output spool. Only the Relay and the runner are played by the test.
#include "RemoteJobsRunnerEnd.h"
#include "storage/BackgroundTaskOutput.h"

#include <QDir>
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

} // namespace

class RemoteJobsEndToEndTest final : public QObject {
    Q_OBJECT

  private slots:
    void aJobRunsFromSubmitToResult() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        const auto submitted = e.h.jobs->record(id);
        QVERIFY(submitted && submitted->snapshot);
        QCOMPARE(submitted->jobId, linkedJob);

        e.assign();
        QCOMPARE(e.h.jobs->record(id)->placement, placement::assigned);

        RunnerEnd end = e.connect(1);
        QCOMPARE(e.parts.out.count(QStringLiteral("signal.answer")), 1);
        QVERIFY(e.play(end, [&] { return e.resultAcked; }));

        QCOMPARE(e.seenTypes.mid(0, 2), (QStringList{"welcome", "job.spec"}));
        const auto specs = framesOf<protocol::JobSpec>(e.seen);
        QCOMPARE(specs.size(), 1);
        QCOMPARE(specs.first().cwd, QStringLiteral("src"));
        QCOMPARE(specs.first().snapshotId, submitted->snapshot->snapshotId);
        QCOMPARE(specs.first().invocation.argv, std::optional(QStringList{"make"}));
        const auto offers = framesOf<protocol::SnapshotOffer>(e.seen);
        QCOMPARE(offers.size(), 1);
        QCOMPARE(offers.first().snapshotId, submitted->snapshot->snapshotId);
        QVERIFY(!e.pack.isEmpty());

        const auto record = e.h.jobs->record(id);
        QVERIFY(record && record->terminal());
        QCOMPARE(record->state, state::completed);
        QCOMPARE(e.h.f.state(id), state::completed);
        QCOMPARE(outboxOf(e.h.f, id), 1);
        const ketplus::daemon::BackgroundOutputPage page =
            ketplus::daemon::BackgroundTaskOutput(e.h.f.database).read(id);
        QCOMPARE(page.bytes, e.output);
        QCOMPARE(page.totalBytes, qint64(e.output.size()));
    }

    // A restart while the snapshot goes: the runner comes back, the desktop resumes the job
    // without a spec and goes on with the same snapshot from the record.
    void aRestartWhileSyncingResumesTheSameSnapshot() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        const QString snapshotId = e.h.jobs->record(id)->snapshot->snapshotId;
        e.assign();
        RunnerEnd first = e.connect(1);
        QVERIFY(e.play(
            first, [&] { return !framesOf<protocol::SnapshotOffer>(e.seen).isEmpty(); }, false));
        QCOMPARE(e.h.jobs->record(id)->placement, placement::syncing);

        e.h.build();
        e.h.jobs->restore();
        const auto restored = e.h.jobs->record(id);
        QVERIFY(restored && !restored->terminal());
        QCOMPARE(restored->snapshot->snapshotId, snapshotId);
        QCOMPARE(e.h.relay.sent(QStringLiteral("cancel")), 0);

        e.seen.clear();
        e.seenTypes.clear();
        RunnerEnd second = e.connect(2);
        QVERIFY(e.play(second, [&] { return e.resultAcked; }));
        QCOMPARE(e.seenTypes.mid(0, 2), (QStringList{"welcome", "job.resume"}));
        QVERIFY(framesOf<protocol::JobSpec>(e.seen).isEmpty());
        const auto offers = framesOf<protocol::SnapshotOffer>(e.seen);
        QVERIFY(!offers.isEmpty());
        QCOMPARE(offers.first().snapshotId, snapshotId);
        QCOMPARE(e.h.f.state(id), state::completed);
        QCOMPARE(outboxOf(e.h.f, id), 1);
    }

    // The store of a syncing job is gone after a restart: the snapshot fails, nothing hangs.
    void aMissingStoreAfterARestartFailsTheSnapshot() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        e.assign();
        RunnerEnd first = e.connect(1);
        QVERIFY(e.play(
            first, [&] { return !framesOf<protocol::SnapshotOffer>(e.seen).isEmpty(); }, false));

        e.h.build();
        QVERIFY(QDir(e.h.stores->storesRoot()).removeRecursively());
        QVERIFY(QDir(e.h.state.filePath("packs")).removeRecursively());
        e.h.jobs->restore();

        e.seen.clear();
        e.seenTypes.clear();
        RunnerEnd second = e.connect(2);
        QVERIFY(e.play(
            second, [&] { return !framesOf<protocol::JobCancel>(e.seen).isEmpty(); }, false));
        const auto stopping = e.h.jobs->record(id);
        QVERIFY(stopping);
        QCOMPARE(stopping->state, state::stopping);
        QCOMPARE(stopping->stopReason, QStringLiteral("snapshot_failed"));
        QVERIFY(!e.ready);

        protocol::JobResult result;
        result.jobId = linkedJob;
        result.state = QStringLiteral("cancelled");
        result.cleanupStatus = QStringLiteral("verified");
        second.send(result);
        QVERIFY(e.play(second, [&] { return e.resultAcked; }, false));
        const auto ended = e.h.jobs->record(id);
        QVERIFY(ended && ended->terminal());
        QCOMPARE(ended->state, state::failed);
        QCOMPARE(ended->reason, QStringLiteral("snapshot_failed"));
        QCOMPARE(e.h.f.state(id), state::failed);
        QCOMPARE(outboxOf(e.h.f, id), 1);
    }

    // The connection drops while output flows: the runner reconnects and the desktop asks for
    // the output from the byte it holds.
    void aLostConnectionResumesOutputAtItsOffset() {
        EndToEnd e;
        const QString id = e.submit();
        QVERIFY(!id.isEmpty());
        e.assign();
        RunnerEnd first = e.connect(1);
        QVERIFY(e.play(first, [&] { return e.ready; }, false));
        QCOMPARE(e.h.jobs->record(id)->placement, placement::syncing);
        const QString now = utc(e.parts.clock.time);
        first.send(protocol::JobStarted{linkedJob, now});
        e.started = true;
        first.send(protocol::JobOutput{linkedJob, 0, QByteArrayLiteral("abc")});
        QCOMPARE(e.h.jobs->record(id)->placement, placement::running);
        first.lose();

        e.seen.clear();
        e.seenTypes.clear();
        RunnerEnd second = e.connect(2);
        QVERIFY(e.play(
            second, [&] { return !framesOf<protocol::JobResume>(e.seen).isEmpty(); }, false));
        QCOMPARE(framesOf<protocol::JobResume>(e.seen).first().outputOffset, 3);
        second.send(protocol::JobOutput{linkedJob, 3, QByteArrayLiteral("def")});
        protocol::JobResult result;
        result.jobId = linkedJob;
        result.state = QStringLiteral("exited");
        result.exitCode = 0;
        result.cleanupStatus = QStringLiteral("verified");
        result.outputBytes = 6;
        result.startedAt = now;
        result.endedAt = now;
        second.send(result);
        QVERIFY(e.play(second, [&] { return e.resultAcked; }, false));
        QCOMPARE(e.h.f.state(id), state::completed);
        QCOMPARE(ketplus::daemon::BackgroundTaskOutput(e.h.f.database).read(id).bytes,
                 QByteArrayLiteral("abcdef"));
    }
};

QTEST_GUILESS_MAIN(RemoteJobsEndToEndTest)
#include "RemoteJobsEndToEndTest.moc"
