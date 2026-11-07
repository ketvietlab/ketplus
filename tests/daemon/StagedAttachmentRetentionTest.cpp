#include "storage/LocalJournal.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
using namespace ketplus::daemon;
namespace ketplus::daemon::detail {
QString stagedManifestGcQuery();
QString stagedChunkGcQuery();
QString stagedRemainingChunksQuery();
} // namespace ketplus::daemon::detail
namespace {
class Fixture {
  public:
    QTemporaryDir dir;
    QString path = dir.filePath("journal.sqlite");
    LocalJournal journal{path};
    QString name = QUuid::createUuid().toString();
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    Fixture() {
        db.setDatabaseName(path);
        if (!db.open() || !journal.isOpen())
            qFatal("fixture failed");
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
        for (const auto& candidate : QSqlDatabase::connectionNames())
            if (candidate.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(candidate).databaseName() == path)
                return QSqlDatabase::database(candidate);
        return {};
    }
    bool seed(int manifests, int chunks, int bytes, bool expired = true) {
        const auto date = expired ? "2000-01-01" : "2099-01-01";
        if (!exec(
                QString("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<%1) "
                        "INSERT INTO staged_attachments SELECT "
                        "'s-'||x,1,'a',1,'file','text/plain',%2,%3,"
                        "'digest','uploading','%4' FROM n")
                    .arg(manifests)
                    .arg(chunks * bytes)
                    .arg(chunks)
                    .arg(date)))
            return false;
        return exec(
            QString("WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<%1) "
                    "INSERT INTO staged_attachment_chunks SELECT "
                    "a.session_id,1,'a',1,x,'digest',zeroblob(%2) "
                    "FROM staged_attachments a CROSS JOIN n")
                .arg(chunks - 1)
                .arg(bytes));
    }
    bool begin(const QString& id = "new") {
        QString error;
        const bool ok = journal.beginAttachment("new-session", 1, id, 0, "file", "text/plain", 1, 1,
                                                QString(64, '0'), &error);
        if (!ok)
            qWarning() << error;
        return ok;
    }
};
} // namespace
class StagedAttachmentRetentionTest : public QObject {
    Q_OBJECT
  private slots:
    void beginDoesNotDrainWholeExpiredBacklog() {
        Fixture f;
        QVERIFY(f.seed(160, 640, 512));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.begin());
        const auto elapsed = timer.elapsed();
        const auto removed = 102400 - f.scalar("SELECT count(*) FROM staged_attachment_chunks");
        qInfo() << "Attachment expiry removed=" << removed << "ms=" << elapsed;
        QVERIFY(removed > 0 && removed <= 128);
        QVERIFY(elapsed < 1000);
    }
    void invalidBeginDoesNotRunMaintenance() {
        Fixture f;
        QVERIFY(f.seed(2, 2, 16));
        QString error;
        QVERIFY(!f.journal.beginAttachment("new-session", 1, "", 0, "file", "text/plain", 1, 1,
                                           QString(64, '0'), &error));
        QCOMPARE(error, QString("invalid_attachment_manifest"));
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 4);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments"), 2);
    }
    void unexpiredBacklogOnlyAdvancesBoundedCandidates() {
        Fixture f;
        QVERIFY(f.seed(160, 640, 512, false));
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 102400);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments"), 160);
        QCOMPARE(f.scalar("SELECT manifest_row FROM journal_attachment_gc"), 32);
        QCOMPARE(f.scalar("SELECT chunk_row FROM journal_attachment_gc"), 128);
    }
    void retirementReservesIdentityUntilEveryChunkIsGone() {
        Fixture f;
        QVERIFY(f.seed(1, 64, 32768));
        QVERIFY(
            f.exec("UPDATE staged_attachments SET digest_sha256=printf('%064d',0),status='ready'"));
        QString error;
        QVERIFY(!f.journal.beginAttachment("s-1", 1, "a", 0, "file", "text/plain", 64 * 32768, 64,
                                           QString(64, '0'), &error));
        QCOMPARE(error, QString("attachment_conflict"));
        const auto remaining = f.scalar("SELECT count(*) FROM staged_attachment_chunks");
        QVERIFY(remaining > 0 && remaining < 64);
        QVERIFY((64 - remaining) * 32768 <= 1024 * 1024);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE status='expiring'"), 1);
        QVERIFY(f.journal.attachment("s-1", 1, "a").id.isEmpty());
        QVERIFY(!f.journal.commitAttachment("s-1", 1, "a", 1, &error));
        QCOMPARE(error, QString("attachment_not_found"));
        {
            LocalJournal reopened(f.path);
            QVERIFY(reopened.isOpen());
            QVERIFY(reopened.attachment("s-1", 1, "a").id.isEmpty());
            for (int i = 0; i < 8; ++i)
                QVERIFY(reopened.purgeExpiredAttachments());
        }
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments"), 0);
        QVERIFY(f.journal.beginAttachment("s-1", 1, "a", 0, "new", "text/plain", 1, 1,
                                          QString(64, '0'), &error));
        QVERIFY(f.journal.attachment("s-1", 1, "a").bytes.isEmpty());
    }
    void expiryIsEnforcedBeforeTheSweepReachesAnIdentity_data() {
        QTest::addColumn<QString>("operation");
        for (const auto& operation :
             {QString("begin"), QString("read"), QString("commit"), QString("chunk")})
            QTest::newRow(qPrintable(operation)) << operation;
    }
    void expiryIsEnforcedBeforeTheSweepReachesAnIdentity() {
        QFETCH(QString, operation);
        Fixture f;
        QVERIFY(f.seed(41, 1, 16));
        QVERIFY(f.exec("UPDATE staged_attachments SET digest_sha256=printf('%064d',0)"));
        QVERIFY(f.exec("UPDATE staged_attachments SET status='ready' WHERE session_id='s-40'"));
        QVERIFY(f.exec("DELETE FROM staged_attachment_chunks WHERE session_id='s-41'"));
        QString error;
        bool began = false;
        if (operation == "begin")
            began = f.journal.beginAttachment("s-40", 1, "a", 0, "file", "text/plain", 16, 1,
                                              QString(64, '0'), &error);
        else
            QVERIFY(f.journal.purgeExpiredAttachments());
        // Only 32 manifests were visited. The target has no retirement marker yet.
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE session_id='s-40' AND "
                          "status='ready'"),
                 1);
        if (operation == "begin") {
            QVERIFY(!began);
            QCOMPARE(error, QString("attachment_conflict"));
        } else if (operation == "read") {
            QVERIFY(f.journal.attachment("s-40", 1, "a").id.isEmpty());
        } else if (operation == "commit") {
            QVERIFY(!f.journal.commitAttachment("s-40", 1, "a", 1, &error));
            QCOMPARE(error, QString("attachment_not_found"));
        } else {
            const QByteArray bytes(16, '\0');
            const auto digest = QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
            QVERIFY(!f.journal.appendAttachmentChunk("s-41", 1, "a", 1, 0, bytes, digest, &error));
            QCOMPARE(
                f.scalar("SELECT count(*) FROM staged_attachment_chunks WHERE session_id='s-41'"),
                0);
        }
    }
    void currentAndUnknownParentsProtectEveryRevision() {
        Fixture f;
        QVERIFY(f.seed(1, 2, 16));
        QVERIFY(f.exec("INSERT INTO staged_attachments SELECT session_id,2,attachment_id,7,name,"
                       "media_type,total_bytes,chunk_count,digest_sha256,'ready','2099-01-01' FROM "
                       "staged_attachments"));
        QVERIFY(f.exec("INSERT INTO staged_attachment_chunks SELECT session_id,2,attachment_id,"
                       "99,chunk_index,digest_sha256,payload FROM staged_attachment_chunks"));
        QVERIFY(f.exec("INSERT INTO staged_attachments SELECT 'unknown',generation,attachment_id,"
                       "revision,name,media_type,total_bytes,chunk_count,digest_sha256,"
                       "'future-format',created_at FROM staged_attachments WHERE generation=1"));
        QVERIFY(
            f.exec("INSERT INTO staged_attachment_chunks SELECT 'unknown',generation,attachment_id,"
                   "revision,chunk_index,digest_sha256,payload FROM staged_attachment_chunks WHERE "
                   "generation=1"));
        QVERIFY(f.exec(
            "INSERT INTO staged_attachment_chunks VALUES('orphan',1,'a',1,0,'digest',x'01')"));
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 4);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks WHERE generation=2"), 2);
        QCOMPARE(
            f.scalar("SELECT count(*) FROM staged_attachment_chunks WHERE session_id='unknown'"),
            2);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments"), 2);
    }
    void quotaCountsUsableDraftsWhileExpiryCatchesUp_data() {
        QTest::addColumn<bool>("expired");
        QTest::newRow("expired-does-not-block") << true;
        QTest::newRow("current-still-blocks") << false;
    }
    void quotaCountsUsableDraftsWhileExpiryCatchesUp() {
        QFETCH(bool, expired);
        Fixture f;
        QVERIFY(f.seed(40, 1, 16, false));
        QVERIFY(f.exec(
            QString("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<5) "
                    "INSERT INTO staged_attachments SELECT "
                    "'target',1,'old-'||x,1,'file','text/plain',16,1,"
                    "'digest','uploading','%1' FROM n")
                .arg(expired ? "2000-01-01" : "2099-01-01")));
        QString error;
        const bool accepted = f.journal.beginAttachment(
            "target", 1, "fresh", 0, "file", "text/plain", 1, 1, QString(64, '0'), &error);
        QCOMPARE(accepted, expired);
        if (!expired)
            QCOMPARE(error, QString("attachment_draft_limit"));
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE session_id='target'"),
                 expired ? 6 : 5);
    }
    void unsupportedChunksKeepIdentityButAllowOtherCleanup() {
        Fixture f;
        QVERIFY(f.seed(2, 1, 16));
        QVERIFY(
            f.exec("UPDATE staged_attachment_chunks SET payload=zeroblob(1100000) WHERE rowid=1"));
        QVERIFY(
            f.exec("UPDATE staged_attachment_chunks SET payload='unsupported-text' WHERE rowid=2"));
        QVERIFY(f.exec(
            "INSERT INTO staged_attachment_chunks VALUES('orphan',1,'a',1,0,'digest',x'01')"));
        for (int i = 0; i < 3; ++i)
            QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 2);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE status='expiring'"), 2);
    }
    void failedPassRollsBackMarkersChunksAndCursors_data() {
        QTest::addColumn<QString>("trigger");
        for (const auto& action : {QString("RAISE(ABORT,'injected')"), QString("RAISE(IGNORE)")}) {
            for (const auto& target :
                 {QString("BEFORE UPDATE ON staged_attachments"),
                  QString("BEFORE DELETE ON staged_attachment_chunks WHEN old.chunk_index=1"),
                  QString("BEFORE DELETE ON staged_attachments"),
                  QString("BEFORE UPDATE ON journal_attachment_gc")})
                QTest::newRow(qPrintable(target + action))
                    << target + " BEGIN SELECT " + action + "; END";
        }
    }
    void failedPassRollsBackMarkersChunksAndCursors() {
        QFETCH(QString, trigger);
        Fixture f;
        QVERIFY(f.seed(1, 2, 16));
        QVERIFY(f.exec("CREATE TRIGGER fail_expiry " + trigger));
        QVERIFY(!f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE status='uploading'"), 1);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 2);
        QCOMPARE(f.scalar("SELECT manifest_row+chunk_row FROM journal_attachment_gc"), 0);
        QVERIFY(f.exec("DROP TRIGGER fail_expiry"));
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments"), 0);
    }
    void commitFailureRollsBackTheWholePass() {
        Fixture f;
        QVERIFY(f.seed(1, 2, 16));
        QVERIFY(f.exec("CREATE TABLE expiry_parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(f.exec("CREATE TABLE expiry_child(id INTEGER REFERENCES expiry_parent(id) "
                       "DEFERRABLE INITIALLY DEFERRED)"));
        QVERIFY(f.exec("CREATE TRIGGER fail_expiry AFTER UPDATE ON journal_attachment_gc BEGIN "
                       "INSERT INTO expiry_child VALUES(1); END"));
        QVERIFY(!f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE status='uploading'"), 1);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 2);
        QCOMPARE(f.scalar("SELECT manifest_row+chunk_row FROM journal_attachment_gc"), 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM expiry_child"), 0);
        QVERIFY(f.exec("DROP TRIGGER fail_expiry"));
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 0);
    }
    void busyWriterDefersAndRestoresTimeout() {
        Fixture f;
        QVERIFY(f.seed(1, 2, 16));
        QSqlQuery q(f.connection());
        QVERIFY(q.exec("PRAGMA busy_timeout=4321"));
        q.finish();
        QVERIFY(f.exec("BEGIN IMMEDIATE"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal.purgeExpiredAttachments());
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 2);
        QCOMPARE(f.scalar("SELECT manifest_row+chunk_row FROM journal_attachment_gc"), 0);
        QVERIFY(q.exec("PRAGMA busy_timeout"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 4321);
        q.finish();
        QVERIFY(f.exec("COMMIT"));
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 0);
    }
    void missingByteLengthCapabilityDefers() {
        Fixture f;
        QVERIFY(f.seed(1, 2, 16));
        QSqlQuery q(f.connection());
        QVERIFY(q.exec("CREATE TEMP TABLE pragma_function_list(name TEXT)"));
        q.finish();
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 2);
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachments WHERE status='uploading'"), 1);
        QCOMPARE(f.scalar("SELECT manifest_row+chunk_row FROM journal_attachment_gc"), 0);
        QVERIFY(q.exec("DROP TABLE temp.pragma_function_list"));
        q.finish();
        QVERIFY(f.journal.purgeExpiredAttachments());
        QCOMPARE(f.scalar("SELECT count(*) FROM staged_attachment_chunks"), 0);
    }
    void actualQueryPlansSeekCandidatesAndFullIdentity() {
        Fixture f;
        const auto plan = [&](const QString& sql, const QVariantList& values) {
            QSqlQuery q(f.db);
            q.prepare("EXPLAIN QUERY PLAN " + sql);
            for (const auto& value : values)
                q.addBindValue(value);
            if (!q.exec())
                return q.lastError().text();
            QString result;
            while (q.next())
                result += q.value(3).toString() + '\n';
            return result;
        };
        const auto manifests = plan(detail::stagedManifestGcQuery(), {0, 32});
        QVERIFY2(manifests.contains("SEARCH a USING INTEGER PRIMARY KEY (rowid>?)"),
                 qPrintable(manifests));
        const auto chunks = plan(detail::stagedChunkGcQuery(), {0, 128});
        QVERIFY2(chunks.contains("SEARCH c USING INTEGER PRIMARY KEY (rowid>?)"),
                 qPrintable(chunks));
        QVERIFY2(chunks.contains("(session_id=? AND generation=? AND attachment_id=?)"),
                 qPrintable(chunks));
        const auto remaining = plan(detail::stagedRemainingChunksQuery(), {1});
        QVERIFY2(remaining.contains("(session_id=? AND generation=? AND attachment_id=?)"),
                 qPrintable(remaining));
        for (const auto& value : {manifests, chunks, remaining})
            QVERIFY2(!value.contains("TEMP B-TREE"), qPrintable(value));
    }
};
QTEST_GUILESS_MAIN(StagedAttachmentRetentionTest)
#include "StagedAttachmentRetentionTest.moc"
