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
};

QTEST_GUILESS_MAIN(RemoteJobsEndToEndTest)
#include "RemoteJobsEndToEndTest.moc"
