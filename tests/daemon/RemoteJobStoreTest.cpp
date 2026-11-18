// One record store contract, run on the memory store and on the store in the background task
// registry; both give the same results.
#include "RemoteJobLedgerFixture.h"
#include "RemoteJobStoreContract.h"
#include "runner/jobs/RemoteJobRecordJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <algorithm>

using namespace remote_job_store_test;

namespace {

struct Fixture {
    virtual ~Fixture() = default;
    virtual StoreUnderTest under() = 0;
};

// The memory store; reopening sees the same records.
struct MemoryFixture final : Fixture {
    MemoryRecordStore memory;
    int prepared = 0;
    StoreUnderTest under() override {
        return {&memory, [this](const bool fail) { memory.failWrites = fail; },
                [this] { return static_cast<RecordStore*>(&memory); },
                [this] {
                    return QStringLiteral("0b6f7c1e-2d4a-4f39-9a51-2c1f0e7d8a%1")
                        .arg(++prepared, 2, 10, QLatin1Char('0'));
                }};
    }
};

// The registry's store; reopening is a new connection to the same file.
struct RegistryFixture final : Fixture {
    remote_job_ledger_test::LedgerFixture ledger;
    StoreUnderTest under() override {
        return {ledger.store.get(), [this](const bool fail) { ledger.failRemoteWrites(fail); },
                [this] { return static_cast<RecordStore*>(ledger.reopen()); },
                [this] { return ledger.remoteTask(); }};
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
    void initTestCase_data() {
        QTest::addColumn<bool>("registry");
        QTest::newRow("memory") << false;
        QTest::newRow("registry") << true;
    }
    void init() {
        QFETCH_GLOBAL(bool, registry);
        fixture_.reset();
        if (registry)
            fixture_ = std::make_unique<RegistryFixture>();
        else
            fixture_ = std::make_unique<MemoryFixture>();
        under_ = fixture_->under();
        QVERIFY(under_.store);
        taskA = under_.prepare();
        taskB = under_.prepare();
        QVERIFY(!taskA.isEmpty() && !taskB.isEmpty());
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
    std::unique_ptr<Fixture> fixture_;
    StoreUnderTest under_;
    QString taskA;
    QString taskB;
};

QTEST_GUILESS_MAIN(RemoteJobStoreTest)
#include "RemoteJobStoreTest.moc"
