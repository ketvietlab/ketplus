#include "storage/LocalJournal.h"
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <memory>

namespace ketplus::daemon::detail {
QString largePayloadGcRawLookup();
QString largePayloadGcReplayLookup();
} // namespace ketplus::daemon::detail

using namespace ketplus::daemon;
namespace {
class Database {
  public:
    explicit Database(const QString& path) : name_(QUuid::createUuid().toString()) {
        db_ = QSqlDatabase::addDatabase("QSQLITE", name_);
        db_.setDatabaseName(path);
        if (!db_.open())
            qFatal("Cannot open fixture");
    }
    ~Database() {
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool exec(const QString& sql) {
        QSqlQuery q(db_);
        if (q.exec(sql))
            return true;
        qWarning() << q.lastError();
        return false;
    }
    qint64 scalar(const QString& sql) {
        QSqlQuery q(db_);
        return q.exec(sql) && q.next() ? q.value(0).toLongLong() : -1;
    }
    QString plan(const QString& sql, const QVariantList& bindings) {
        QSqlQuery q(db_);
        q.prepare("EXPLAIN QUERY PLAN " + sql);
        for (const auto& value : bindings)
            q.addBindValue(value);
        if (!q.exec())
            return q.lastError().text();
        QString result;
        while (q.next())
            result += q.value(3).toString() + '\n';
        return result;
    }
    bool payloads(int count, int chunks = 0, int first = 0) {
        if (!db_.transaction())
            return false;
        for (int i = first; i < first + count; ++i) {
            QSqlQuery q(db_);
            q.prepare("INSERT INTO large_event_payloads "
                      "(session_id,generation,transfer_id,event_type,item_id,revision,digest_"
                      "sha256,payload,created_at,total_bytes) "
                      "VALUES('s',1,?,'item.completed',?,1,'digest',X'','2020-01-01',?)");
            q.addBindValue(QString("transfer-%1").arg(i));
            q.addBindValue(QString("item-%1").arg(i));
            q.addBindValue(chunks * 131072);
            if (!q.exec())
                return false;
            for (int n = 0; n < chunks; ++n) {
                QSqlQuery c(db_);
                c.prepare(
                    "INSERT INTO large_event_payload_chunks VALUES('s',1,?,?,zeroblob(131072))");
                c.addBindValue(QString("transfer-%1").arg(i));
                c.addBindValue(n * 131072);
                if (!c.exec())
                    return false;
            }
        }
        return db_.commit();
    }

  private:
    QString name_;
    QSqlDatabase db_;
};
bool rawBacklog(Database& db) {
    return db.exec(
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
        "INSERT INTO events(session_id,generation,cursor,event_type,payload,sent_at,turn_sequence) "
        "SELECT 's',1,x,'item.delta',json_object('text',printf('%1000s','x')),'2020-01-01',30 FROM "
        "n");
}
} // namespace
class JournalGarbageCollectionTest : public QObject {
    Q_OBJECT
  private slots:
    void candidateAndByteBudgetsEventuallyFinish() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        Database db(path);
        QVERIFY(db.payloads(40, 9));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 8);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 352);
        QCOMPARE(db.scalar("SELECT payload_row FROM journal_gc_state"), 32);
        QCOMPARE(db.scalar("SELECT chunk_row FROM journal_gc_state"), 8);
        int passes = 1;
        for (; passes < 60 && db.scalar("SELECT count(*) FROM large_event_payload_chunks");
             ++passes) {
            const auto before =
                db.scalar("SELECT sum(length(payload)) FROM large_event_payload_chunks");
            QVERIFY(journal.pruneLargePayloadGarbage());
            const auto after = db.scalar(
                "SELECT coalesce(sum(length(payload)),0) FROM large_event_payload_chunks");
            QVERIFY(before - after <= 1024 * 1024);
        }
        qInfo() << "40 payloads / 45 MiB collected in" << passes << "passes";
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT payload_row+chunk_row FROM journal_gc_state"), 0);
    }
    void chunkCandidateLimitIncludesProtectedRows() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(1, 129));
        QVERIFY(db.exec("INSERT INTO events VALUES('s',1,1,'item.delta','{}','2020-01-01',1)"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT chunk_row FROM journal_gc_state"), 128);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 129);
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT chunk_row FROM journal_gc_state"), 129);
    }
    void cursorsSurviveReopenAndWrap() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        auto journal = std::make_unique<LocalJournal>(path);
        Database db(path);
        QVERIFY(db.payloads(80));
        QVERIFY(db.exec("INSERT INTO events VALUES('s',1,1,'item.delta','{}','2020-01-01',1)"));
        QVERIFY(journal->pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT payload_row FROM journal_gc_state"), 32);
        journal.reset();
        journal = std::make_unique<LocalJournal>(path);
        QVERIFY(journal->isOpen());
        QCOMPARE(db.scalar("SELECT payload_row FROM journal_gc_state"), 64);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 80);
        QVERIFY(db.exec("DELETE FROM events"));
        for (int pass = 0; pass < 5; ++pass)
            QVERIFY(journal->pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 0);
    }
    void sharedReferencesAndGenerationIsolation() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(3, 1));
        QVERIFY(
            db.exec("UPDATE large_event_payloads SET generation=2 WHERE transfer_id='transfer-2'"));
        QVERIFY(db.exec(
            "UPDATE large_event_payload_chunks SET generation=2 WHERE transfer_id='transfer-2'"));
        QVERIFY(db.exec("INSERT INTO events VALUES('s',2,1,'item.delta','{}','2020-01-01',1)"));
        QVERIFY(db.exec("INSERT INTO replay_events VALUES('s',1,1,'item.completed','item-0',"
                        "'{\"large_event\":{\"transfer_id\":\"transfer-0\"}}','2020-01-01',1),"
                        "('s',1,2,'item.completed','item-0',"
                        "'{\"large_event\":{\"transfer_id\":\"transfer-0\"}}','2020-01-01',2)"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 2);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 2);
        QVERIFY(db.exec("DELETE FROM replay_events WHERE cursor=1"));
        for (int pass = 0; pass < 2; ++pass)
            QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 2);
        QVERIFY(db.exec("DELETE FROM replay_events"));
        for (int pass = 0; pass < 2; ++pass)
            QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 1);
        QCOMPARE(db.scalar("SELECT generation FROM large_event_payloads"), 2);
        QVERIFY(db.exec("DELETE FROM events"));
        for (int pass = 0; pass < 2; ++pass)
            QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
    }
    void legacyAndMalformedRowsDoNotStallFollowingRows() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(4));
        QVERIFY(db.exec("UPDATE large_event_payloads SET payload=zeroblob(1048577) WHERE "
                        "transfer_id='transfer-0'"));
        QVERIFY(db.exec(
            "UPDATE large_event_payloads SET payload='invalid' WHERE transfer_id='transfer-1'"));
        QVERIFY(db.exec("INSERT INTO large_event_payload_chunks VALUES"
                        "('s',1,'orphan',0,zeroblob(131073)),('s',1,'orphan',131073,'invalid'),"
                        "('s',1,'orphan',131080,X'01')"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 2);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 2);
        QCOMPARE(db.scalar("SELECT payload_row FROM journal_gc_state"), 4);
        QCOMPARE(db.scalar("SELECT chunk_row FROM journal_gc_state"), 3);
    }
    void metadataAndChunkBytesShareBudget() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(3));
        QVERIFY(db.exec("UPDATE large_event_payloads SET payload=zeroblob(786432) WHERE "
                        "transfer_id!='transfer-2'"));
        QVERIFY(db.exec(
            "INSERT INTO large_event_payload_chunks VALUES('s',1,'orphan',0,zeroblob(131072)),"
            "('s',1,'orphan',1,zeroblob(131072)),('s',1,'orphan',2,zeroblob(131072))"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT payload_row FROM journal_gc_state"), 1);
        QCOMPARE(db.scalar("SELECT chunk_row FROM journal_gc_state"), 2);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 2);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 1);
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
    }
    void failedPassRollsBackRowsAndCursors_data() {
        QTest::addColumn<QString>("trigger");
        for (const auto& action :
             {QString("RAISE(ABORT,'injected failure')"), QString("RAISE(IGNORE)")}) {
            QTest::newRow(qPrintable("metadata-" + action))
                << "BEFORE DELETE ON large_event_payloads WHEN OLD.transfer_id='transfer-1' BEGIN "
                   "SELECT " +
                       action + "; END";
            QTest::newRow(qPrintable("chunk-" + action))
                << "BEFORE DELETE ON large_event_payload_chunks WHEN OLD.transfer_id='transfer-1' "
                   "BEGIN SELECT " +
                       action + "; END";
            QTest::newRow(qPrintable("cursor-" + action))
                << "BEFORE UPDATE ON journal_gc_state BEGIN SELECT " + action + "; END";
        }
    }
    void failedPassRollsBackRowsAndCursors() {
        QFETCH(QString, trigger);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(3, 2));
        QVERIFY(db.exec("CREATE TRIGGER fail_gc " + trigger));
        QVERIFY(!journal.pruneLargePayloadGarbage());
        QVERIFY(!journal.errorString().isEmpty());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 3);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 6);
        QCOMPARE(db.scalar("SELECT payload_row+chunk_row FROM journal_gc_state"), 0);
        QVERIFY(db.exec("DROP TRIGGER fail_gc"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
    }
    void commitFailureRollsBackRowsAndCursors() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(3, 2));
        QVERIFY(db.exec("CREATE TABLE gc_fault_parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(db.exec("CREATE TABLE gc_fault_child(id INTEGER REFERENCES gc_fault_parent(id) "
                        "DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(db.exec("CREATE TRIGGER gc_fault AFTER UPDATE ON journal_gc_state BEGIN INSERT "
                        "INTO gc_fault_child VALUES(1); END"));
        QVERIFY(!journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 3);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 6);
        QCOMPARE(db.scalar("SELECT payload_row+chunk_row FROM journal_gc_state"), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM gc_fault_child"), 0);
        QVERIFY(db.exec("DROP TRIGGER gc_fault"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
    }
    void walReaderKeepsSnapshotWithoutBlockingGc() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(3, 2));
        QVERIFY(db.exec("BEGIN"));
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 6);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(journal.pruneLargePayloadGarbage());
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 6);
        QVERIFY(db.exec("COMMIT"));
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
        QCOMPARE(db.scalar("SELECT payload_row FROM journal_gc_state"), 3);
    }
    void busyWriterDefersWithoutAdvancing() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(2, 1));
        QSqlDatabase connection;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                connection = QSqlDatabase::database(name);
        QVERIFY(connection.isValid());
        QSqlQuery timeout(connection);
        QVERIFY(timeout.exec("PRAGMA busy_timeout=4321"));
        timeout.finish();
        QVERIFY(db.exec("BEGIN IMMEDIATE"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(journal.pruneLargePayloadGarbage());
        QVERIFY(timer.elapsed() < 500);
        QVERIFY(journal.isOpen());
        QVERIFY(timeout.exec("PRAGMA busy_timeout"));
        QVERIFY(timeout.next());
        QCOMPARE(timeout.value(0).toInt(), 4321);
        timeout.finish();
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 2);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 2);
        QCOMPARE(db.scalar("SELECT payload_row+chunk_row FROM journal_gc_state"), 0);
        QVERIFY(db.exec("COMMIT"));
        QVERIFY(journal.pruneLargePayloadGarbage());
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payload_chunks"), 0);
    }
    void productionLookupsUseAllIndexKeys() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        const auto raw = db.plan(detail::largePayloadGcRawLookup(), {QString("s"), 1});
        QVERIFY2(raw.contains("SEARCH") && raw.contains("session_id=? AND generation=?"),
                 qPrintable(raw));
        const auto replay = db.plan(detail::largePayloadGcReplayLookup(),
                                    {QString("s"), 1, QString("item"), QString("transfer")});
        QVERIFY2(replay.contains("replay_events_item_transfer"), qPrintable(replay));
        QVERIFY2(replay.contains("session_id=? AND generation=? AND item_id=? AND <expr>=?"),
                 qPrintable(replay));
    }
    void retainedBacklogDoesNotBlockGlobalGc() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        Database db(path);
        QVERIFY(db.payloads(32));
        QVERIFY(db.exec("UPDATE large_event_payloads SET item_id='shared'"));
        QVERIFY(
            db.exec("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<300000) "
                    "INSERT INTO replay_events SELECT 's',1,x,'item.completed','shared',"
                    "json_object('large_event',json_object('transfer_id','other-'||x)),'2020-01-01'"
                    ",x FROM n"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(journal.pruneLargePayloadGarbage());
        qInfo() << "Global GC with 300000 same-item replay rows:" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 0);
        QCOMPARE(db.scalar("SELECT count(*) FROM replay_events"), 300000);
    }
    void pendingRawBacklogDoesNotBlockGlobalGc_data() {
        QTest::addColumn<bool>("reopen");
        QTest::newRow("reopen") << true;
        QTest::newRow("command-result") << false;
    }
    void pendingRawBacklogDoesNotBlockGlobalGc() {
        QFETCH(bool, reopen);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        {
            LocalJournal initial(path);
            QVERIFY(initial.isOpen());
        }
        Database db(path);
        // The existing object lets the command-result case isolate the 256th write.
        auto journal = std::make_unique<LocalJournal>(path);
        QVERIFY(journal->isOpen());
        QVERIFY(db.payloads(32));
        QVERIFY(rawBacklog(db));
        if (!reopen)
            for (int i = 0; i < 255; ++i)
                QVERIFY(journal->storeCommandResult(QString::number(i), {}));
        QElapsedTimer timer;
        timer.start();
        if (reopen) {
            journal.reset();
            journal = std::make_unique<LocalJournal>(path);
        } else
            QVERIFY(journal->storeCommandResult("last", {}));
        const auto elapsed = timer.elapsed();
        QVERIFY2(journal->isOpen(), qPrintable(journal->errorString()));
        qInfo() << "Global GC with 100000 pending raw rows, reopen=" << reopen << elapsed << "ms";
        QVERIFY2(elapsed < 1000, "Global GC scanned the pending raw backlog");
        QCOMPARE(db.scalar("SELECT count(*) FROM large_event_payloads"), 32);
        QCOMPARE(db.scalar("SELECT count(*) FROM events"), 100000);
    }
};
QTEST_GUILESS_MAIN(JournalGarbageCollectionTest)
#include "JournalGarbageCollectionTest.moc"
