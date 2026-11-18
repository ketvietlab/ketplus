#include "RemoteJobStoreContract.h"
#include "runner/jobs/RemoteJobRecordJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QTest>

#include <algorithm>

using namespace remote_job_store_test;

namespace {

const QString taskA = QStringLiteral("0b6f7c1e-2d4a-4f39-9a51-2c1f0e7d8a01");
const QString taskB = QStringLiteral("0b6f7c1e-2d4a-4f39-9a51-2c1f0e7d8a02");

// The memory store; reopening sees the same records.
struct MemoryFixture {
    MemoryRecordStore memory;
    StoreUnderTest under() {
        return {&memory, [this](const bool fail) { memory.failWrites = fail; },
                [this] { return static_cast<RecordStore*>(&memory); }, [](const QString&) {}};
    }
};

std::optional<RemoteJobRecord> find(const RecordStore& store, const QString& taskId) {
    const QList<RemoteJobRecord> all = store.load();
    const auto found = std::find_if(all.cbegin(), all.cend(), [&taskId](const auto& record) {
        return record.taskId == taskId;
    });
    return found == all.cend() ? std::nullopt : std::optional(*found);
}

RemoteJobRecord stored(RemoteJobRecord record, const qint64 revision) {
    record.revision = revision;
    return record;
}

} // namespace

class RemoteJobStoreTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        fixture_ = std::make_unique<MemoryFixture>();
        under_ = fixture_->under();
        under_.prepare(taskA);
        under_.prepare(taskB);
    }

    void aNewRecordIsStoredWithRevisionOne() {
        RecordStore& store = *under_.store;
        QVERIFY(store.load().isEmpty());
        QVERIFY(store.put(newRecord(taskA), 0));
        QCOMPARE(find(store, taskA), std::optional(stored(newRecord(taskA), 1)));
        QVERIFY(!store.put(newRecord(taskA), 0)); // it exists now
        QCOMPARE(find(store, taskA)->revision, 1);
    }

    void aRecordThatDoesNotExistNeedsZero() {
        QVERIFY(!under_.store->put(newRecord(taskA), 1));
        QVERIFY(under_.store->load().isEmpty());
    }

    void eachPutNeedsTheStoredRevision_data() {
        QTest::addColumn<qint64>("expected");
        QTest::addColumn<bool>("taken");
        QTest::newRow("current") << qint64(2) << true;
        QTest::newRow("behind") << qint64(1) << false;
        QTest::newRow("ahead") << qint64(3) << false;
        QTest::newRow("new") << qint64(0) << false;
    }
    void eachPutNeedsTheStoredRevision() {
        QFETCH(qint64, expected);
        QFETCH(bool, taken);
        RecordStore& store = *under_.store;
        QVERIFY(store.put(newRecord(taskA), 0));
        RemoteJobRecord queued = newRecord(taskA);
        queued.placement = placement::relayQueued;
        queued.jobId = QStringLiteral("job-a");
        QVERIFY(store.put(queued, 1));
        RemoteJobRecord next = queued;
        next.state = state::starting;
        next.placement = placement::assigned;
        QCOMPARE(store.put(next, expected), taken);
        QCOMPARE(find(store, taskA), std::optional(taken ? stored(next, 3) : stored(queued, 2)));
    }

    void aFailedWriteChangesNothing() {
        RecordStore& store = *under_.store;
        QVERIFY(store.put(newRecord(taskA), 0));
        under_.failWrites(true);
        RemoteJobRecord changed = newRecord(taskA);
        changed.state = state::cancelled;
        changed.reason = QStringLiteral("user");
        QVERIFY(!store.put(changed, 1));
        QVERIFY(!store.put(newRecord(taskB), 0));
        under_.failWrites(false);
        QCOMPARE(store.load(), QList<RemoteJobRecord>{stored(newRecord(taskA), 1)});
        QVERIFY(store.put(changed, 1)); // the same write goes through once the store works
        QCOMPARE(find(store, taskA)->state, state::cancelled);
    }

    void everyFieldReadsBackAfterAReopen() {
        QVERIFY(under_.store->put(newRecord(taskA), 0));
        QVERIFY(under_.store->put(fullRecord(taskA), 1));
        RemoteJobRecord done = newRecord(taskB);
        done.state = state::failed;
        done.reason = QStringLiteral("queue_expired");
        QVERIFY(under_.store->put(done, 0));
        RecordStore& reopened = *under_.reopen();
        QCOMPARE(find(reopened, taskA), std::optional(stored(fullRecord(taskA), 2)));
        // A terminal record is still loaded.
        QCOMPARE(find(reopened, taskB), std::optional(stored(done, 1)));
        QCOMPARE(reopened.load().size(), 2);
    }

    void unsetOptionalsStayUnset() {
        RemoteJobRecord record = newRecord(taskA);
        record.runner = RemoteRunner{QStringLiteral("runner-1"), {}, 1};
        QVERIFY(under_.store->put(record, 0));
        const auto read = find(*under_.reopen(), taskA);
        QVERIFY(read);
        QVERIFY(!read->dispatchedAt && !read->lastPeerContact && !read->startedAt);
        QVERIFY(!read->exitCode && !read->signal);
        QCOMPARE(read->runner, record.runner);
    }

    void theStoredFormReadsBack() {
        RemoteJobRecord full = fullRecord(taskA);
        QCOMPARE(decodeRecord(encodeRecord(full)), std::optional(full));
        RemoteJobRecord bare = newRecord(taskB);
        QCOMPARE(decodeRecord(encodeRecord(bare)), std::optional(bare));
        QVERIFY(!encodeRecord(full).contains("revision"));
    }

    void aStoredFormThatIsNotARecordIsRefused_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::newRow("version") << "version" << QJsonValue(2);
        QTest::newRow("no task") << "taskId" << QJsonValue("");
        QTest::newRow("state") << "state" << QJsonValue("paused");
        QTest::newRow("placement") << "placement" << QJsonValue("elsewhere");
        QTest::newRow("cleanup") << "cleanupStatus" << QJsonValue("best_effort_complete");
        QTest::newRow("timeout type") << "timeoutMs" << QJsonValue("60000");
        QTest::newRow("negative timeout") << "timeoutMs" << QJsonValue(-1);
        QTest::newRow("labels") << "runsOn" << QJsonValue("os:linux");
        QTest::newRow("label type") << "runsOn" << QJsonValue(QJsonArray{1});
        QTest::newRow("time type") << "startedAt" << QJsonValue("today");
        QTest::newRow("fraction") << "exitCode" << QJsonValue(1.5);
        QTest::newRow("runner") << "runner" << QJsonValue("runner-1");
        QTest::newRow("runner id") << "runner" << QJsonValue(QJsonObject{{"attempt", 1}});
    }
    void aStoredFormThatIsNotARecordIsRefused() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QJsonObject object = QJsonDocument::fromJson(encodeRecord(fullRecord(taskA))).object();
        object.insert(field, value);
        QCOMPARE(decodeRecord(QJsonDocument(object).toJson()), std::nullopt);
        QCOMPARE(decodeRecord("{not json"), std::nullopt);
        QCOMPARE(decodeRecord("[]"), std::nullopt);
    }

  private:
    std::unique_ptr<MemoryFixture> fixture_;
    StoreUnderTest under_;
};

QTEST_GUILESS_MAIN(RemoteJobStoreTest)
#include "RemoteJobStoreTest.moc"
