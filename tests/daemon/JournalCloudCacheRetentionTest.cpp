#include "storage/LocalJournal.h"
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <limits>

using namespace ketplus::daemon;
namespace ketplus::daemon::detail {
QString cloudCacheInvalidationQuery();
QString metadataRetentionScanQuery(const QString& table);
} // namespace ketplus::daemon::detail
namespace {
class Fixture {
  public:
    QTemporaryDir dir;
    QString path = dir.filePath("cache.sqlite");
    LocalJournal journal{path};
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    Fixture() {
        db.setDatabaseName(path);
        if (!journal.isOpen() || !db.open() ||
            !journal.upsertSession({"cache-s", "fixture", "codex", "thread", {}, "ready", 1, 0, 1}))
            qFatal("Synthetic cache setup failed");
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
    QSqlDatabase connection() const {
        for (const auto& n : QSqlDatabase::connectionNames())
            if (n.startsWith("ketplus-daemon-") && QSqlDatabase::database(n).databaseName() == path)
                return QSqlDatabase::database(n);
        return {};
    }
    bool seed(int count, int bytes = 1000) {
        return exec(QString("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n "
                            "WHERE x<%1) INSERT INTO "
                            "cloud_events(session_id,generation,cursor,event,created_at,projection_"
                            "policy_version) SELECT 'cache-s',1,x,"
                            "json_object('text',printf('%*s',%2,'x')),'2026-01-01',2 FROM n")
                        .arg(count)
                        .arg(bytes));
    }
    bool ack(qint64 cursor, const QString& client = "cloud-control-v2", qint64 generation = 1) {
        if (!journal.acknowledgeCursor(client, "cache-s", generation, cursor))
            return false;
        // Large fixtures model receipts already recorded for these exact rows.
        // Production receipt transitions are exercised separately below.
        return client != "cloud-control-v2" ||
               exec(QString("UPDATE cloud_events SET upload_acked=1 WHERE session_id='cache-s' "
                            "AND generation=%1 AND cursor<=%2")
                        .arg(generation)
                        .arg(cursor));
    }
    bool receipt(const QJsonArray& envelopes, qint64 acceptedThrough) {
        return journal.acknowledgeCloudEventUpload("fixture-scope", "cache-s", 1, envelopes,
                                                   acceptedThrough);
    }
    qint64 count() { return scalar("SELECT count(*) FROM cloud_events"); }
};
QJsonObject envelope(qint64 cursor, const QString& content = "original") {
    return {{"sessionId", "cache-s"},
            {"sessionGeneration", 1},
            {"cursor", cursor},
            {"content", QJsonObject{{"ciphertext", content}}}};
}

} // namespace
class JournalCloudCacheRetentionTest : public QObject {
    Q_OBJECT
  private slots:
    void ackBacklogUsesOneBoundedPass() {
        Fixture f;
        QVERIFY(f.seed(100000));
        QVERIFY(f.ack(100000));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        qInfo() << "ACK cache removed" << 100000 - f.count() << "rows in" << timer.elapsed()
                << "ms";
        QCOMPARE(f.count(), 99744);
        QCOMPARE(f.scalar("SELECT min(cursor) FROM cloud_events"), 257);
    }
    void oldCloudCursorCannotAuthorizeRegeneratedCache() {
        Fixture f;
        QVERIFY(f.seed(1));
        QVERIFY(f.ack(1));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, {{"fresh", true}}));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 1);
    }
    void legacyCacheWithoutReceiptRemainsPendingOnMigration() {
        Fixture f;
        QVERIFY(f.seed(1));
        QVERIFY(f.journal.acknowledgeCursor("cloud-control-v2", "cache-s", 1, 1));
        QVERIFY(f.exec("ALTER TABLE cloud_events DROP COLUMN upload_acked"));
        LocalJournal reopened(f.path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(f.count(), 1);
        QCOMPARE(f.scalar("SELECT upload_acked FROM cloud_events"), 0);
        QVERIFY(reopened.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 1);
    }
    void byteBudgetAndReopen() {
        Fixture f;
        QVERIFY(f.seed(3, 400000));
        QVERIFY(f.ack(3));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 1);
        LocalJournal reopened(f.path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(f.count(), 0);
    }
    void onlyMatchingCloudAckIsEligible() {
        Fixture f;
        QVERIFY(f.seed(5));
        QVERIFY(f.ack(5, "desktop"));
        QVERIFY(f.ack(5, "cloud-control-v2", 2));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 5);
        QVERIFY(f.ack(2));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 3);
        QCOMPARE(f.scalar("SELECT min(cursor) FROM cloud_events"), 3);
    }
    void idleMaintenanceReclaimsAcceptedOnly() {
        Fixture f;
        QVERIFY(f.seed(300));
        QVERIFY(f.ack(280));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.count(), 44);
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.count(), 20);
    }
    void unsupportedAndOversizedRowsDoNotBlockOtherCache() {
        Fixture f;
        QVERIFY(f.seed(300));
        QVERIFY(f.ack(300));
        QVERIFY(f.exec("UPDATE cloud_events SET event=zeroblob(100) WHERE cursor=1"));
        QVERIFY(f.exec("UPDATE cloud_events SET event=printf('%*s',2097152,'x') WHERE cursor=2"));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 46);
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 2);
        QVERIFY(!f.journal.discardCloudEvent("cache-s", 1, 1));
        QVERIFY(!f.journal.discardCloudEvent("cache-s", 1, 2));
        QCOMPARE(f.count(), 2);
    }
    void invalidationIsExactAcrossBacklogAndGenerations() {
        Fixture f;
        QVERIFY(f.seed(100000));
        QVERIFY(f.exec("INSERT INTO "
                       "cloud_events(session_id,generation,cursor,event,created_at,projection_"
                       "policy_version) VALUES('cache-s',2,99999,'{}','2026-01-01',2)"));
        QVERIFY(f.exec("INSERT INTO "
                       "cloud_events(session_id,generation,cursor,event,created_at,projection_"
                       "policy_version) VALUES('other',1,99999,'{}','2026-01-01',2)"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal.discardCloudEvent("cache-s", 1, 99999));
        qInfo() << "Exact cache invalidation over 100000 rows:" << timer.elapsed() << "ms";
        QCOMPARE(f.count(), 100001);
        QVERIFY(!f.journal.cloudEvent("cache-s", 1, 99998).isEmpty());
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 99999, {{"fresh", true}}));
        QCOMPARE(f.journal.cloudEvent("cache-s", 1, 99999).value("fresh").toBool(), true);
        QCOMPARE(f.journal.cloudEventProjectionPolicyVersion("cache-s", 1, 99999), 2);
    }
    void pendingUploadRetainsCacheUntilItsOwnReceipt() {
        Fixture f;
        const auto e = envelope(1);
        const QJsonArray batch{e};
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, e));
        QVERIFY(f.receipt(batch, 1));
        QCOMPARE(f.scalar("SELECT upload_acked FROM cloud_events"), 1);
        QVERIFY(f.journal.prepareCloudEventUpload(batch));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 1);
        QCOMPARE(f.journal.cloudEvent("cache-s", 1, 1), e);
        QVERIFY(f.receipt(batch, 1));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
    }
    void oldReceiptCannotAcknowledgeRegeneratedCiphertext() {
        Fixture f;
        const auto old = envelope(1);
        const auto fresh = envelope(1, "regenerated");
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, old));
        QVERIFY(f.journal.prepareCloudEventUpload({old}));
        QVERIFY(f.journal.discardCloudEvent("cache-s", 1, 1));
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, fresh));
        QVERIFY(f.receipt({old}, 1));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 1);
        QCOMPARE(f.journal.cloudEvent("cache-s", 1, 1), fresh);
        QVERIFY(!f.journal.prepareCloudEventUpload({old}));
        QVERIFY(f.journal.prepareCloudEventUpload({fresh}));
    }
    void receiptsFenceCursorAndScope() {
        Fixture f;
        const auto one = envelope(1);
        const auto two = envelope(2);
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, one));
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 2, two));
        auto wrong = one;
        wrong.insert("sessionGeneration", 2);
        QVERIFY(!f.receipt({wrong}, 2));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 2);
        QVERIFY(f.receipt({one, two}, 1));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 1);
        QCOMPARE(f.journal.cloudEvent("cache-s", 1, 2), two);
    }
    void ignoredReceiptOrPendingWritesCannotReportSuccess() {
        Fixture f;
        const QJsonArray batch{envelope(1), envelope(2)};
        for (const auto& value : batch) {
            const auto e = value.toObject();
            QVERIFY(f.journal.storeCloudEvent("cache-s", 1, e.value("cursor").toInteger(), e));
        }
        QVERIFY(
            f.exec("CREATE TRIGGER ignore_receipt BEFORE UPDATE ON cloud_events WHEN OLD.cursor=2 "
                   "BEGIN SELECT RAISE(IGNORE); END"));
        QVERIFY(!f.receipt(batch, 2));
        QCOMPARE(f.scalar("SELECT sum(upload_acked) FROM cloud_events"), 0);
        QVERIFY(f.exec("DROP TRIGGER ignore_receipt"));
        QVERIFY(f.receipt(batch, 2));
        QVERIFY(
            f.exec("CREATE TRIGGER ignore_pending BEFORE UPDATE ON cloud_events WHEN OLD.cursor=2 "
                   "BEGIN SELECT RAISE(IGNORE); END"));
        QVERIFY(!f.journal.prepareCloudEventUpload(batch));
        QCOMPARE(f.scalar("SELECT sum(upload_acked) FROM cloud_events"), 2);
    }
    void receiptCommitFailureRollsBack() {
        Fixture f;
        const auto e = envelope(1);
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, e));
        QVERIFY(f.exec("CREATE TABLE receipt_parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(f.exec("CREATE TABLE receipt_child(id INTEGER REFERENCES receipt_parent(id) "
                       "DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(f.exec("CREATE TRIGGER fail_receipt AFTER UPDATE ON cloud_events "
                       "BEGIN INSERT INTO receipt_child VALUES(1); END"));
        QVERIFY(!f.receipt({e}, 1));
        QCOMPARE(f.scalar("SELECT upload_acked FROM cloud_events"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM receipt_child"), 0);
        QCOMPARE(f.journal.cloudUploadCursor("fixture-scope", "cache-s", 1), 0);
        QVERIFY(f.exec("DROP TRIGGER fail_receipt"));
        QVERIFY(f.receipt({e}, 1));
    }
    void receiptAndScopedProgressCommitTogether() {
        Fixture f;
        const auto e = envelope(1);
        QVERIFY(f.journal.storeCloudEvent("cache-s", 1, 1, e));
        QVERIFY(f.exec("CREATE TRIGGER ignore_progress BEFORE INSERT ON cloud_upload_progress "
                       "BEGIN SELECT RAISE(IGNORE); END"));
        QVERIFY(!f.receipt({e}, 1));
        QCOMPARE(f.scalar("SELECT upload_acked FROM cloud_events"), 0);
        QCOMPARE(f.journal.cloudUploadCursor("fixture-scope", "cache-s", 1), 0);
        QVERIFY(f.exec("DROP TRIGGER ignore_progress"));
        QVERIFY(f.receipt({e}, 1));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
        LocalJournal reopened(f.path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.cloudUploadCursor("fixture-scope", "cache-s", 1), 1);
        QCOMPARE(reopened.cloudUploadCursor("other-workspace", "cache-s", 1), 0);
        QCOMPARE(reopened.cloudUploadCursor("fixture-scope", "cache-s", 2), 0);
        QVERIFY(f.exec("UPDATE cloud_upload_progress SET cursor='unsupported'"));
        QCOMPARE(reopened.cloudUploadCursor("fixture-scope", "cache-s", 1), -1);
        QVERIFY(!reopened.acknowledgeCloudEventUpload("fixture-scope", "cache-s", 1, {e}, 1));
    }
    void receiptRowsAndBytesAreBounded() {
        Fixture f;
        QJsonArray tooMany;
        for (int i = 0; i < 101; ++i)
            tooMany.append(envelope(i + 1));
        QVERIFY(!f.journal.prepareCloudEventUpload(tooMany));
        QVERIFY(!f.receipt(tooMany, 101));
        QJsonArray large;
        for (int i = 1; i <= 3; ++i) {
            const auto e = envelope(i, QString(400000, 'x'));
            QVERIFY(f.journal.storeCloudEvent("cache-s", 1, i, e));
            large.append(e);
        }
        QVERIFY(!f.receipt(large, 3));
        QCOMPARE(f.scalar("SELECT sum(upload_acked) FROM cloud_events"), 0);
    }
    void missingAndInvalidIdentity() {
        Fixture f;
        QVERIFY(f.seed(1));
        QVERIFY(f.journal.discardCloudEvent("cache-s", 1, 2));
        QVERIFY(!f.journal.discardCloudEvent("", 1, 1));
        QVERIFY(!f.journal.discardCloudEvent("cache-s", 0, 1));
        QVERIFY(!f.journal.discardCloudEvent("cache-s", 1, 0));
        QCOMPARE(f.count(), 1);
    }
    void failedDeleteRollsBack_data() {
        QTest::addColumn<bool>("maintenance");
        QTest::addColumn<QString>("failure");
        for (bool maintenance : {false, true})
            for (const auto& failure : {QString("IGNORE"), QString("ABORT,'fixture'")})
                QTest::newRow(qPrintable(QString("%1-%2").arg(maintenance).arg(failure)))
                    << maintenance << failure;
    }
    void failedDeleteRollsBack() {
        QFETCH(bool, maintenance);
        QFETCH(QString, failure);
        Fixture f;
        QVERIFY(f.seed(3));
        QVERIFY(f.ack(3));
        QVERIFY(f.exec("CREATE TRIGGER fail_cache BEFORE DELETE ON cloud_events WHEN OLD.cursor=2 "
                       "BEGIN SELECT RAISE(" +
                       failure + "); END"));
        const bool ok = maintenance ? f.journal.pruneAcknowledgedCloudEvents()
                                    : f.journal.discardCloudEvent("cache-s", 1, 2);
        QVERIFY(!ok);
        QCOMPARE(f.count(), 3);
        QVERIFY(f.exec("DROP TRIGGER fail_cache"));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
    }
    void commitFailureRollsBack_data() {
        QTest::addColumn<bool>("maintenance");
        QTest::newRow("maintenance") << true;
        QTest::newRow("invalidation") << false;
    }
    void commitFailureRollsBack() {
        QFETCH(bool, maintenance);
        Fixture f;
        QVERIFY(f.seed(3));
        QVERIFY(f.ack(3));
        QVERIFY(f.exec("CREATE TABLE fault_parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(f.exec("CREATE TABLE fault_child(id INTEGER REFERENCES fault_parent(id) "
                       "DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(f.exec("CREATE TRIGGER fail_commit AFTER DELETE ON cloud_events "
                       "BEGIN INSERT INTO fault_child VALUES(1); END"));
        const bool ok = maintenance ? f.journal.pruneAcknowledgedCloudEvents()
                                    : f.journal.discardCloudEvent("cache-s", 1, 2);
        QVERIFY(!ok);
        QCOMPARE(f.count(), 3);
        QCOMPARE(f.scalar("SELECT count(*) FROM fault_child"), 0);
        QVERIFY(f.exec("DROP TRIGGER fail_commit"));
        QVERIFY(f.journal.discardCloudEvent("cache-s", 1, 2));
        QCOMPARE(f.count(), 2);
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
    }
    void busyWriterDefersAndRestoresTimeout() {
        Fixture f;
        QVERIFY(f.seed(3));
        QVERIFY(f.ack(3));
        QSqlQuery q(f.connection());
        QVERIFY(q.exec("PRAGMA busy_timeout=4321"));
        QVERIFY(f.exec("BEGIN IMMEDIATE"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QVERIFY(!f.journal.discardCloudEvent("cache-s", 1, 2));
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(f.count(), 3);
        QVERIFY(q.exec("PRAGMA busy_timeout"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 4321);
        q.finish();
        QVERIFY(f.exec("COMMIT"));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
    }
    void missingCapabilityDefersMaintenance() {
        Fixture f;
        QVERIFY(f.seed(3));
        QVERIFY(f.ack(3));
        QSqlQuery q(f.connection());
        QVERIFY(q.exec("CREATE TEMP TABLE pragma_function_list(name TEXT)"));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 3);
        QVERIFY(q.exec("DROP TABLE temp.pragma_function_list"));
        QVERIFY(f.journal.pruneAcknowledgedCloudEvents());
        QCOMPARE(f.count(), 0);
    }
    void actualQueriesUseExactIdentityAndBoundedGcSeeks() {
        Fixture f;
        QSqlQuery q(f.db);
        QVERIFY(q.prepare("EXPLAIN QUERY PLAN " + detail::cloudCacheInvalidationQuery()));
        q.addBindValue("cache-s");
        q.addBindValue(1);
        q.addBindValue(42);
        QVERIFY(q.exec());
        QString plan;
        while (q.next())
            plan += q.value(3).toString() + '\n';
        QVERIFY2(
            plan.contains(
                "sqlite_autoindex_cloud_events_1 (session_id=? AND generation=? AND cursor=?)"),
            qPrintable(plan));
        QVERIFY(
            q.prepare("EXPLAIN QUERY PLAN " + detail::metadataRetentionScanQuery("cloud_events")));
        q.addBindValue(0);
        q.addBindValue(std::numeric_limits<qint64>::max());
        q.addBindValue(256);
        QVERIFY(q.exec());
        plan.clear();
        while (q.next())
            plan += q.value(3).toString() + '\n';
        QVERIFY2(plan.contains("SEARCH t USING INTEGER PRIMARY KEY (rowid>? AND rowid<?)"),
                 qPrintable(plan));

        QVERIFY2(!plan.contains("TEMP B-TREE"), qPrintable(plan));
    }
};
QTEST_GUILESS_MAIN(JournalCloudCacheRetentionTest)
#include "JournalCloudCacheRetentionTest.moc"
