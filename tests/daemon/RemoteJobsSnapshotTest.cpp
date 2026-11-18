// The snapshots of remote jobs in real stores of a real repository through RemoteJobs: each task
// holds its own snapshot, a submit the Relay refuses lets its snapshot go, and the hourly sweep
// clears the pack cache.
#include "RemoteJobsRunnerEnd.h"
#include "runner/sync/StoreRetention.h"

#include <QDir>
#include <QFile>
#include <QTest>

using namespace remote_jobs_test;

namespace {

// The object an in-flight ref of `taskId` points at, or empty when there is no such ref.
QString heldBy(const QStringList& refs, const QString& taskId) {
    const QString name = sync::StoreRetention::inFlightRef(taskId);
    for (const QString& line : refs) {
        const QStringList parts = line.split(QLatin1Char(' '));
        if (parts.size() == 2 && parts.first().endsWith(name))
            return parts.last();
    }
    return {};
}

} // namespace

class RemoteJobsSnapshotTest final : public QObject {
    Q_OBJECT

  private slots:
    void twoTasksInOneRepositoryKeepTheirOwnSnapshots() {
        EndToEnd e;
        const QString first = e.submit(60'000, QStringLiteral("job_1"));
        QVERIFY(!first.isEmpty());
        QVERIFY(e.edit("int edited = 2;\n"));
        const QString second = e.submit(60'000, QStringLiteral("job_2"));
        QVERIFY(!second.isEmpty());

        const auto a = e.h.jobs->record(first)->snapshot;
        const auto b = e.h.jobs->record(second)->snapshot;
        QVERIFY(a && b);
        QCOMPARE(a->repositoryId, b->repositoryId);
        QVERIFY(a->snapshotId != b->snapshotId);
        QStringList refs = e.storeRefs(first);
        QVERIFY(!heldBy(refs, first).isEmpty());
        QVERIFY(!heldBy(refs, second).isEmpty());
        QVERIFY(heldBy(refs, first) != heldBy(refs, second));
        const QString kept = heldBy(refs, second);

        // The first task ends: only its snapshot goes.
        QVERIFY(e.h.jobs->stop(first).ok());
        remote_job_test::RelayJob cancelled =
            remote_job_test::relayJob(QStringLiteral("cancelled"));
        cancelled.jobId = QStringLiteral("job_1");
        QVERIFY(e.h.relay.answer(QStringLiteral("cancel"), cancelled));
        QVERIFY(e.h.jobs->record(first)->terminal());
        QCOMPARE(e.h.run(), 1); // the end of the record runs on a later turn
        QVERIFY(heldBy(e.storeRefs(second), first).isEmpty());
        QCOMPARE(heldBy(e.storeRefs(second), second), kept);
    }

    void aSubmitTheRelayRefusesLetsItsSnapshotGo() {
        EndToEnd e;
        const QString id = e.submit(60'000, {});
        QVERIFY(!id.isEmpty());
        QVERIFY(!heldBy(e.storeRefs(id), id).isEmpty());
        QVERIFY(e.h.relay.refuse(QStringLiteral("submit"), QStringLiteral("queue_full")));
        const auto ended = e.h.jobs->record(id);
        QVERIFY(ended && ended->terminal());
        QCOMPARE(e.h.f.state(id), ended->state);
        QVERIFY(!heldBy(e.storeRefs(id), id).isEmpty());
        e.h.run();
        QVERIFY(heldBy(e.storeRefs(id), id).isEmpty());
    }

    void theSweepClearsOldPacks() {
        EndToEnd e;
        const QString packs = e.h.state.filePath(QStringLiteral("packs"));
        QVERIFY(QDir().mkpath(packs));
        const QString stale = QDir(packs).filePath(QStringLiteral("stale.pack"));
        QFile file(stale);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("pack") == 4);
        file.close();
        e.h.jobs->sweep(QDateTime::currentDateTimeUtc().addDays(30));
        QTRY_VERIFY(!QFile::exists(stale));
    }
};

QTEST_GUILESS_MAIN(RemoteJobsSnapshotTest)
#include "RemoteJobsSnapshotTest.moc"
