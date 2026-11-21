// Remote jobs in the running daemon: the snapshot kept in a record, a record that cannot be read
// after a restart, and which Relay socket messages are the runner links'.
#include "RemoteJobLedgerFixture.h"
#include "runner/jobs/RemoteJobRecordJson.h"
#include "runner/remote/RelayMessages.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace remote_job_ledger_test;
using ketplus::daemon::runner::remote::RelayRoute;
using ketplus::daemon::runner::remote::relayRoute;

namespace {

RemoteJobRecord firstRecord(const QString& taskId) {
    RemoteJobRecord record;
    record.taskId = taskId;
    record.runsOn = linuxLabels;
    record.timeoutMs = 60'000;
    return record;
}

RemoteSnapshot aSnapshot() {
    return RemoteSnapshot{QStringLiteral("/repo/.git"),
                          QStringLiteral("/repo"),
                          QStringLiteral("0123456789abcdef0123456789abcdef01234567"),
                          {QStringLiteral("submodules_present")}};
}

int outboxOf(LedgerFixture& f, const QString& taskId) {
    return f.count("SELECT count(*) FROM managed_job_outbox WHERE task_id='" + taskId + "'");
}

} // namespace

class RemoteJobsWiringTest final : public QObject {
    Q_OBJECT

  private slots:
    void aRecordKeepsItsSnapshot() {
        RemoteJobRecord record = firstRecord(QStringLiteral("task-1"));
        record.snapshot = aSnapshot();
        const auto decoded = decodeRecord(encodeRecord(record));
        QVERIFY(decoded);
        QCOMPARE(decoded->snapshot, record.snapshot);
    }

    void aRecordWithoutASnapshotStillReads() {
        const RemoteJobRecord record = firstRecord(QStringLiteral("task-1"));
        const QByteArray bytes = encodeRecord(record);
        QVERIFY(!bytes.contains("snapshot"));
        const auto decoded = decodeRecord(bytes);
        QVERIFY(decoded);
        QVERIFY(!decoded->snapshot);
    }

    void aMalformedSnapshotMakesTheRecordUnreadable() {
        RemoteJobRecord record = firstRecord(QStringLiteral("task-1"));
        record.snapshot = aSnapshot();
        QJsonObject object = QJsonDocument::fromJson(encodeRecord(record)).object();
        QJsonObject snapshot = object.value("snapshot").toObject();
        snapshot.insert("warnings", 3);
        object.insert("snapshot", snapshot);
        QVERIFY(!decodeRecord(QJsonDocument(object).toJson()));
        snapshot.insert("warnings", QJsonArray{});
        snapshot.remove("snapshotId");
        object.insert("snapshot", snapshot);
        QVERIFY(!decodeRecord(QJsonDocument(object).toJson()));
    }

    // The task ends once, its row reads again as ended, and a second restart tells no one.
    void anUnreadableRecordEndsOnceAndIsNotLoadedAgain() {
        LedgerFixture f;
        const QString id = f.remoteTask();
        RemoteJobRecord record = firstRecord(id);
        QVERIFY(f.store->put(record, 0));
        record.jobId = QStringLiteral("job-1");
        record.placement = placement::relayQueued;
        QVERIFY(f.store->put(record, 1));
        QVERIFY(f.exec("UPDATE remote_jobs SET record='{not json' WHERE task_id='" + id + "'"));
        QTest::ignoreMessage(QtWarningMsg, qPrintable("remote job record unreadable task=" + id));
        QVERIFY(f.reopen()->load().isEmpty());
        QCOMPARE(f.store->unreadable(), QStringList{id});

        QTest::ignoreMessage(QtWarningMsg, qPrintable("remote job record abandoned task=" + id));
        const auto jobId = f.store->abandonUnreadable(id);
        QVERIFY(jobId);
        QCOMPARE(*jobId, QStringLiteral("job-1"));
        QCOMPARE(f.state(id), state::interrupted);
        QCOMPARE(f.task(id).value("reason").toString(), QStringLiteral("record_unreadable"));
        QCOMPARE(f.task(id).value("cleanup_status").toString(), cleanup::unverified);
        QCOMPARE(outboxOf(f, id), 1);

        const auto loaded = f.reopen()->load();
        QVERIFY(f.store->unreadable().isEmpty());
        QCOMPARE(loaded.size(), 1);
        QVERIFY(loaded.first().terminal());
        QCOMPARE(loaded.first().reason, QStringLiteral("record_unreadable"));
        QCOMPARE(outboxOf(f, id), 1);
    }

    void aRecordThatReadsIsNeverAbandoned() {
        LedgerFixture f;
        const QString id = f.remoteTask();
        QVERIFY(f.store->put(firstRecord(id), 0));
        QVERIFY(!f.store->abandonUnreadable(id));
        QVERIFY(!f.store->abandonUnreadable(f.remoteTask())); // reserved, no record yet
        QCOMPARE(f.state(id), state::queued);
        QCOMPARE(outboxOf(f, id), 0);
    }

    void runnerMessagesLeaveAndEveryOtherStays() {
        const auto owns = [](const QString& id) { return id == QStringLiteral("runner-conn"); };
        const QJsonObject runnerOffer{{"type", "signal.offer"},
                                      {"connectionId", "runner-conn"},
                                      {"runnerId", "runner-1"},
                                      {"jobId", "job-1"}};
        QCOMPARE(relayRoute(runnerOffer, owns), RelayRoute::RunnerLink);
        const QJsonObject deviceOffer{
            {"type", "signal.offer"}, {"connectionId", "device-conn"}, {"deviceId", "device-1"}};
        QCOMPARE(relayRoute(deviceOffer, owns), RelayRoute::Device);
        QJsonObject withGrant = runnerOffer;
        withGrant.insert("grant", "g");
        QCOMPARE(relayRoute(withGrant, owns), RelayRoute::Device);
        for (const char* type : {"signal.answer", "signal.candidate", "signal.end", "signal.error"})
            QCOMPARE(relayRoute({{"type", type}, {"connectionId", "device-conn"}}, owns),
                     RelayRoute::Device);
        for (const char* type : {"signal.candidate", "signal.end", "signal.error"})
            QCOMPARE(relayRoute({{"type", type}, {"connectionId", "runner-conn"}}, owns),
                     RelayRoute::RunnerLink);
        // An answer is never the runner links': the desktop only answers.
        QCOMPARE(relayRoute({{"type", "signal.answer"}, {"connectionId", "runner-conn"}}, owns),
                 RelayRoute::Device);
        QCOMPARE(relayRoute({{"type", "job.changed"}, {"jobId", "job-1"}}, owns),
                 RelayRoute::JobChanged);
        QCOMPARE(relayRoute({{"type", "job.changed"}}, owns), RelayRoute::Device);
        QCOMPARE(relayRoute({{"type", "wake"}}, owns), RelayRoute::Device);
        QCOMPARE(relayRoute({{"type", "signal.end"}, {"connectionId", "runner-conn"}}, {}),
                 RelayRoute::Device);
    }
};

QTEST_GUILESS_MAIN(RemoteJobsWiringTest)
#include "RemoteJobsWiringTest.moc"
