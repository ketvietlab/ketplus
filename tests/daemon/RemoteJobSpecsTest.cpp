// The in-memory spec source of remote jobs: kept once per task, bound to the job id and
// attempt asked for, dropped when the record ends, and empty after a restart.
#include "runner/jobs/RemoteJobSpecs.h"

#include <QTest>

using namespace ketplus::daemon::runner::jobs;
namespace protocol = ketplus::daemon::runner::peer::protocol;

namespace {

const QString taskA = QStringLiteral("0b6f7c1e-2d4a-4f39-9a51-2c1f0e7d8a01");
const QString taskB = QStringLiteral("0b6f7c1e-2d4a-4f39-9a51-2c1f0e7d8a02");

protocol::JobSpec specOf(const QString& marker) {
    protocol::JobSpec spec;
    spec.invocation.argv = QStringList{QStringLiteral("make"), marker};
    spec.cwd = QStringLiteral("build");
    spec.env = {{QStringLiteral("TOKEN"), marker}, {QStringLiteral("UNSET"), std::nullopt}};
    spec.timeoutMs = 60'000;
    spec.repositoryKey = QStringLiteral("repo-key");
    spec.workspaceKey = QStringLiteral("workspace-key");
    spec.snapshotId = QStringLiteral("snapshot-1");
    spec.objectFormat = QStringLiteral("sha1");
    return spec;
}

RemoteJobRecord recordOf(const QString& taskId, const QString& jobId,
                         const QString& state = state::queued) {
    RemoteJobRecord record;
    record.taskId = taskId;
    record.jobId = jobId;
    record.state = state;
    return record;
}

} // namespace

class RemoteJobSpecsTest final : public QObject {
    Q_OBJECT

  private slots:
    void aKeptSpecIsBoundToTheJobAndAttempt() {
        RemoteJobSpecs specs;
        QVERIFY(specs.store(taskA, specOf("one")));
        QCOMPARE(specs.spec("job-a", 1), std::nullopt); // no record names the job yet
        specs.recorded(recordOf(taskA, {}));
        QCOMPARE(specs.spec("job-a", 1), std::nullopt);
        specs.recorded(recordOf(taskA, "job-a"));
        protocol::JobSpec expected = specOf("one");
        expected.jobId = "job-a";
        expected.attempt = 3;
        QCOMPARE(specs.spec("job-a", 3), std::optional(expected));
        expected.attempt = 4;
        QCOMPARE(specs.spec("job-a", 4), std::optional(expected));
        QCOMPARE(specs.spec("job-b", 1), std::nullopt);
        QCOMPARE(specs.spec({}, 1), std::nullopt);
    }

    void aSpecIsKeptOnce() {
        RemoteJobSpecs specs;
        protocol::JobSpec first = specOf("one");
        QVERIFY(specs.store(taskA, first));
        first.jobId = "ignored";
        first.attempt = 7;
        first.epoch = 9;
        QVERIFY(specs.store(taskA, first)); // the same spec: nothing changes
        QVERIFY(!specs.store(taskA, specOf("two")));
        QVERIFY(!specs.store({}, specOf("one")));
        specs.recorded(recordOf(taskA, "job-a"));
        QCOMPARE(specs.spec("job-a", 1)->env.value("TOKEN"), std::optional<QString>("one"));
        QCOMPARE(specs.spec("job-a", 1)->epoch, 0);
    }

    void aSpecIsDroppedWhenItsRecordEnds() {
        RemoteJobSpecs specs;
        QVERIFY(specs.store(taskA, specOf("one")));
        QVERIFY(specs.store(taskB, specOf("two")));
        specs.recorded(recordOf(taskA, "job-a"));
        specs.recorded(recordOf(taskB, "job-b", state::stopping));
        QVERIFY(specs.spec("job-b", 1)); // not ended
        specs.recorded(recordOf(taskA, "job-a", state::cancelled));
        QCOMPARE(specs.spec("job-a", 1), std::nullopt);
        QVERIFY(!specs.holds(taskA));
        QVERIFY(!specs.store(taskA, specOf("one"))); // an ended task takes none again
        QCOMPARE(specs.size(), 1);
        specs.recorded(recordOf(taskB, {}, state::failed)); // ended before it had a job
        QCOMPARE(specs.size(), 0);
    }

    void aRestartHoldsNoSpec() {
        {
            RemoteJobSpecs specs;
            QVERIFY(specs.store(taskA, specOf("one")));
            specs.recorded(recordOf(taskA, "job-a"));
        }
        RemoteJobSpecs restarted;
        restarted.recorded(recordOf(taskA, "job-a")); // restored records name their jobs
        QCOMPARE(restarted.spec("job-a", 1), std::nullopt);
    }
};

QTEST_GUILESS_MAIN(RemoteJobSpecsTest)
#include "RemoteJobSpecsTest.moc"
