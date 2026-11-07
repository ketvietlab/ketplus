#include "storage/LocalJournal.h"
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <memory>

namespace ketplus::daemon::detail {
QString acknowledgedEventFenceQuery();
QString acknowledgedEventCandidatesQuery();
} // namespace ketplus::daemon::detail
using namespace ketplus::daemon;
namespace {
class Fixture {
  public:
    QTemporaryDir dir;
    QString path = dir.filePath("journal.sqlite");
    std::unique_ptr<LocalJournal> journal = std::make_unique<LocalJournal>(path);
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    Fixture() {
        db.setDatabaseName(path);
        if (!db.open() || !journal->isOpen())
            qFatal("Fixture failed");
    }
    ~Fixture() {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(name);
    }
    bool exec(const QString& sql) {
        QSqlQuery q(db);
        if (q.exec(sql))
            return true;
        qWarning() << q.lastError();
        return false;
    }
    qint64 scalar(const QString& sql) {
        QSqlQuery q(db);
        return q.exec(sql) && q.next() ? q.value(0).toLongLong() : -1;
    }
    bool rows(int count, int bytes = 1000, const QString& session = "s", int generation = 1) {
        QSqlQuery q(db);
        q.prepare(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<?) "
            "INSERT INTO "
            "events(session_id,generation,cursor,event_type,payload,sent_at,turn_sequence) "
            "SELECT ?,?,x,'item.delta',json_object('text',printf('%'||?||'s','x')),'2020-01-01',1 "
            "FROM n");
        q.addBindValue(count);
        q.addBindValue(session);
        q.addBindValue(generation);
        q.addBindValue(bytes);
        return q.exec();
    }
    bool queued(int cursor, const QString& session = "s", int generation = 1) {
        QSqlQuery q(db);
        q.prepare("INSERT INTO client_cursors VALUES('reader',?,?,?,'2020-01-01')");
        q.addBindValue(session);
        q.addBindValue(generation);
        q.addBindValue(cursor);
        if (!q.exec())
            return false;
        q.prepare("INSERT INTO journal_ack_gc(session_id,generation) VALUES(?,?)");
        q.addBindValue(session);
        q.addBindValue(generation);
        return q.exec();
    }
};
} // namespace
class JournalAcknowledgementRetentionTest : public QObject {
    Q_OBJECT
  private slots:
    void productionQueryPlansUseGenerationAndCursorIndexes() {
        Fixture f;
        const auto plan = [&](const QString& sql, const QVariantList& bindings) {
            QSqlQuery q(f.db);
            q.prepare("EXPLAIN QUERY PLAN " + sql);
            for (const auto& value : bindings)
                q.addBindValue(value);
            if (!q.exec())
                return q.lastError().text();
            QString result;
            while (q.next())
                result += q.value(3).toString() + '\n';
            return result;
        };
        const auto fence = plan(detail::acknowledgedEventFenceQuery(), {QString("s"), 1});
        QVERIFY2(fence.contains("client_cursors_generation") &&
                     fence.contains("session_id=? AND generation=?"),
                 qPrintable(fence));
        const auto rows =
            plan(detail::acknowledgedEventCandidatesQuery(), {QString("s"), 1, 0, 100000, 128});
        QVERIFY2(rows.contains("session_id=? AND generation=? AND cursor>? AND cursor<?"),
                 qPrintable(rows));
        QVERIFY2(!rows.contains("SCAN") && !rows.contains("TEMP B-TREE"), qPrintable(rows));
    }
    void backlogAcknowledgementIsBounded() {
        Fixture f;
        QVERIFY(f.rows(100000));
        QVERIFY(f.exec(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<32) "
            "INSERT INTO "
            "large_event_payloads(session_id,generation,transfer_id,event_type,item_id,revision,"
            "digest_sha256,payload,created_at,total_bytes) "
            "SELECT 's',1,'transfer-'||x,'item.completed','item-'||x,1,'digest',X'','2020-01-01',0 "
            "FROM n"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal->acknowledgeCursor("reader", "s", 1, 100000));
        qInfo() << "ACK 100000 raw + 32 metadata:" << timer.elapsed() << "ms";
        QVERIFY(timer.elapsed() < 1000);
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 99872);
        QCOMPARE(f.scalar("SELECT cursor FROM client_cursors"), 100000);
        QCOMPARE(f.scalar("SELECT count(*) FROM large_event_payloads"), 32);
        QCOMPARE(f.scalar("SELECT count(*) FROM journal_ack_gc"), 1);
    }
    void slowestClientAndGenerationAreFenced() {
        Fixture f;
        QVERIFY(f.rows(300));
        QVERIFY(f.rows(300, 1000, "s", 2));
        QVERIFY(f.journal->acknowledgeCursor("slow", "s", 1, 0));
        QVERIFY(f.journal->acknowledgeCursor("fast", "s", 1, 300));
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 600);
        QVERIFY(f.journal->acknowledgeCursor("slow", "s", 1, 200));
        QCOMPARE(f.scalar("SELECT count(*) FROM events WHERE generation=1"), 172);
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events WHERE generation=1"), 100);
        QCOMPARE(f.scalar("SELECT min(cursor) FROM events WHERE generation=1"), 201);
        QCOMPARE(f.scalar("SELECT count(*) FROM events WHERE generation=2"), 300);
        QVERIFY(f.exec("UPDATE client_cursors SET acknowledged_at='marker'"));
        QVERIFY(f.journal->acknowledgeCursor("slow", "s", 1, 200));
        QCOMPARE(f.scalar("SELECT count(*) FROM client_cursors WHERE acknowledged_at='marker'"), 2);
    }
    void queuedFenceIsRecheckedForNewClient() {
        Fixture f;
        QVERIFY(f.rows(300));
        QVERIFY(f.queued(300));
        QVERIFY(f.exec("INSERT INTO client_cursors VALUES('new','s',1,0,'2020-01-01')"));
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 300);
        QVERIFY(f.journal->acknowledgeCursor("new", "s", 1, 300));
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 172);
    }
    void byteBudgetAndReopenResume() {
        Fixture f;
        QVERIFY(f.rows(4, 500000));
        QVERIFY(f.journal->acknowledgeCursor("reader", "s", 1, 4));
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 2);
        QCOMPARE(f.scalar("SELECT after_cursor FROM journal_ack_gc"), 2);
        f.journal.reset();
        f.journal = std::make_unique<LocalJournal>(f.path);
        QVERIFY(f.journal->isOpen());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 0);
        for (int n = 0; n < 2; ++n)
            QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM journal_ack_gc"), 0);
    }
    void oversizedAndBinaryRowsDoNotStarveOtherGenerations() {
        Fixture f;
        QVERIFY(f.rows(3));
        QVERIFY(f.exec("UPDATE events SET payload=printf('%8000000s','x') WHERE cursor=1"));
        QVERIFY(f.exec("UPDATE events SET payload=X'0001' WHERE cursor=2"));
        QVERIFY(f.queued(3));
        QVERIFY(f.rows(2, 20, "other"));
        QVERIFY(f.queued(2, "other"));
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events WHERE session_id='s'"), 2);
        QCOMPARE(f.scalar("SELECT count(*) FROM events WHERE session_id='other'"), 0);
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 2);
    }
    void queueRotationIsFair() {
        Fixture f;
        for (int n = 0; n < 10; ++n) {
            QVERIFY(f.rows(200, 10, QString::number(n)));
            QVERIFY(f.queued(200, QString::number(n)));
        }
        for (int n = 0; n < 10; ++n)
            QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 720);
        QCOMPARE(f.scalar("SELECT max(n) FROM (SELECT count(*) n FROM events GROUP BY session_id)"),
                 72);
        for (int n = 0; n < 30; ++n)
            QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM journal_ack_gc"), 0);
    }
    void cleanupFailureRollsBack_data() {
        QTest::addColumn<QString>("trigger");
        QTest::newRow("raw-abort") << QString(
            "BEFORE DELETE ON events WHEN OLD.cursor=2 BEGIN SELECT RAISE(ABORT,'failure'); END");
        QTest::newRow("raw-ignore")
            << QString("BEFORE DELETE ON events WHEN OLD.cursor=2 BEGIN SELECT RAISE(IGNORE); END");
        QTest::newRow("queue-delete")
            << QString("BEFORE DELETE ON journal_ack_gc BEGIN SELECT RAISE(IGNORE); END");
        QTest::newRow("queue-insert")
            << QString("BEFORE INSERT ON journal_ack_gc BEGIN SELECT RAISE(IGNORE); END");
        QTest::newRow("commit") << QString(
            "AFTER INSERT ON journal_ack_gc BEGIN INSERT INTO fault_child VALUES(1); END");
    }
    void cleanupFailureRollsBack() {
        QFETCH(QString, trigger);
        Fixture f;
        QVERIFY(f.rows(3));
        QVERIFY(f.queued(3));
        QVERIFY(f.exec("CREATE TABLE fault_parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(f.exec("CREATE TABLE fault_child(id INTEGER REFERENCES fault_parent(id) DEFERRABLE "
                       "INITIALLY DEFERRED)"));
        QVERIFY(f.exec("CREATE TRIGGER fault " + trigger));
        QVERIFY(!f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 3);
        QCOMPARE(f.scalar("SELECT queue_id FROM journal_ack_gc"), 1);
        QCOMPARE(f.scalar("SELECT after_cursor FROM journal_ack_gc"), 0);
        QVERIFY(f.exec("DROP TRIGGER fault"));
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 0);
    }
    void queueFailureRejectsAckAtomically_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("abort") << QString("RAISE(ABORT,'failure')");
        QTest::newRow("ignore") << QString("RAISE(IGNORE)");
    }
    void queueFailureRejectsAckAtomically() {
        QFETCH(QString, action);
        Fixture f;
        QVERIFY(f.rows(3));
        QVERIFY(f.exec("CREATE TRIGGER fault BEFORE INSERT ON journal_ack_gc BEGIN SELECT " +
                       action + "; END"));
        QVERIFY(!f.journal->acknowledgeCursor("reader", "s", 1, 3));
        QCOMPARE(f.scalar("SELECT count(*) FROM client_cursors"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM journal_ack_gc"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 3);
    }
    void maintenanceFailureDoesNotRejectCommittedAck() {
        Fixture f;
        QVERIFY(f.rows(3));
        QVERIFY(f.exec("CREATE TRIGGER fault BEFORE DELETE ON events BEGIN SELECT "
                       "RAISE(ABORT,'failure'); END"));
        QVERIFY(f.journal->acknowledgeCursor("reader", "s", 1, 3));
        QCOMPARE(f.scalar("SELECT cursor FROM client_cursors"), 3);
        QCOMPARE(f.scalar("SELECT count(*) FROM journal_ack_gc"), 1);
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 3);
        QVERIFY(f.exec("DROP TRIGGER fault"));
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 0);
    }
    void embeddedNulTextUsesByteLength() {
        Fixture f;
        QVERIFY(f.rows(3));
        QVERIFY(f.exec("UPDATE events SET payload=CAST(zeroblob(600000) AS TEXT)"));
        QVERIFY(f.journal->acknowledgeCursor("reader", "s", 1, 3));
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 2);
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 1);
    }
    void missingLengthCapabilityDefersWithoutLosingQueue() {
        Fixture f;
        QVERIFY(f.rows(3));
        QSqlDatabase connection;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == f.path)
                connection = QSqlDatabase::database(name);
        QSqlQuery q(connection);
        // Model an older SQLite capability listing without changing the system plugin.
        QVERIFY(q.exec("CREATE TEMP TABLE pragma_function_list(name TEXT)"));
        QVERIFY(f.journal->acknowledgeCursor("reader", "s", 1, 3));
        QCOMPARE(f.scalar("SELECT cursor FROM client_cursors"), 3);
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 3);
        QCOMPARE(f.scalar("SELECT count(*) FROM journal_ack_gc"), 1);
        QVERIFY(q.exec("DROP TABLE temp.pragma_function_list"));
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 0);
    }
    void busyWriterDefersAndRestoresTimeout() {
        Fixture f;
        QVERIFY(f.rows(3));
        QVERIFY(f.queued(3));
        QSqlDatabase connection;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == f.path)
                connection = QSqlDatabase::database(name);
        QSqlQuery q(connection);
        QVERIFY(q.exec("PRAGMA busy_timeout=4321"));
        q.finish();
        QVERIFY(f.exec("BEGIN IMMEDIATE"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 3);
        QCOMPARE(f.scalar("SELECT queue_id FROM journal_ack_gc"), 1);
        QVERIFY(q.exec("PRAGMA busy_timeout"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 4321);
        q.finish();
        QVERIFY(f.exec("COMMIT"));
        QVERIFY(f.journal->pruneAcknowledgedEvents());
        QCOMPARE(f.scalar("SELECT count(*) FROM events"), 0);
    }
};
QTEST_GUILESS_MAIN(JournalAcknowledgementRetentionTest)
#include "JournalAcknowledgementRetentionTest.moc"
