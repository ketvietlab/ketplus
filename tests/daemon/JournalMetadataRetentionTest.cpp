#include "storage/LocalJournal.h"
#include <QElapsedTimer>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;
namespace ketplus::daemon::detail {
QString metadataRetentionScanQuery(const QString& table);
QString metadataRetentionCutoffQuery(const QString& table);
QString deletionWorkQuery(bool pendingCloudOnly, bool wrap);
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
    QSqlDatabase connection() const {
        for (const auto& candidate : QSqlDatabase::connectionNames())
            if (candidate.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(candidate).databaseName() == path)
                return QSqlDatabase::database(candidate);
        return {};
    }
    bool expiredCommands(int count, int bytes = 100) {
        return exec(QString("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n "
                            "WHERE x<%1) INSERT INTO command_results SELECT 'expired-'||x,"
                            "printf('%*s',%2,'x'),'2000-01-01T00:00:00.000Z' FROM n")
                        .arg(count)
                        .arg(bytes));
    }
    bool deletionJobs(int count, int bytes = 1000) {
        return exec(
            QString(
                "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<%1) "
                "INSERT INTO session_deletion_work SELECT 'done-'||x,1,"
                "json_object('operationId','done-'||x,'pad',printf('%*s',%2,'x')),'[]',1 FROM n")
                .arg(count)
                .arg(bytes));
    }
    bool pendingDeletion(bool provider) {
        return exec(
            QString("INSERT INTO session_deletion_work "
                    "VALUES('target',1,'{\"operationId\":\"op\"}',%1,%2)")
                .arg(provider ? "'[{\"provider\":\"codex\",\"thread_id\":\"thread\"}]'" : "'[]'")
                .arg(provider ? 1 : 0));
    }
    bool completeDeletion(bool provider) {
        return provider ? journal.completeProviderCleanup("target", 1, "codex", "thread")
                        : journal.acknowledgeCloudDeletion("target", 1, "op");
    }
    bool seed(bool audit, int count) {
        const auto prefix =
            QString("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<%1) ")
                .arg(count);
        return exec(
            prefix +
            (audit ? "INSERT INTO remote_command_audit SELECT "
                     "'old-'||x,'turn.start','device',1,'s','ok','2099-01-01T00:00:00.000Z' FROM n"
                   : "INSERT INTO command_results SELECT "
                     "'old-'||x,json_object('text',printf('%1000s','x')),'2099-01-01T00:00:00.000Z'"
                     " FROM n"));
    }
    bool store(bool audit, const QString& id) {
        return audit ? journal.recordRemoteCommand({id, "turn.start", "device", 1, "s", "ok", {}})
                     : journal.storeCommandResult(id, {{"accepted", true}});
    }
};
} // namespace
class JournalMetadataRetentionTest : public QObject {
    Q_OBJECT
  private slots:
    void clockRollbackKeepsTheNewWrite_data() {
        QTest::addColumn<bool>("audit");
        QTest::newRow("command") << false;
        QTest::newRow("audit") << true;
    }
    void completionDoesNotSweepUnrelatedDeletionJobs_data() {
        QTest::addColumn<bool>("provider");
        QTest::newRow("cloud-ack") << false;
        QTest::newRow("provider-done") << true;
    }
    void completionDoesNotSweepUnrelatedDeletionJobs() {
        QFETCH(bool, provider);
        Fixture f;
        QVERIFY(f.deletionJobs(100000));
        QVERIFY(f.pendingDeletion(provider));
        QVERIFY(f.exec("INSERT INTO session_deletion_work "
                       "VALUES('waiting',1,'{\"operationId\":\"wait\"}','[]',0)"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.completeDeletion(provider));
        qInfo() << "Deletion completion provider=" << provider << "ms=" << timer.elapsed();
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 100002);
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work WHERE session_id='target' "
                          "AND cloud_acked=1 AND provider_targets='[]'"),
                 1);
        const auto pending = f.journal.deletionWork();
        QCOMPARE(pending.size(), 1);
        QCOMPARE(
            pending.first().toObject().value("deletion").toObject().value("operationId").toString(),
            QString("wait"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 100002 - 256);
        QCOMPARE(
            f.scalar("SELECT cloud_acked FROM session_deletion_work WHERE session_id='waiting'"),
            0);
    }
    void completedDeletionCleanupHonorsByteBudget() {
        Fixture f;
        QVERIFY(f.deletionJobs(3, 400000));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 1);
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 0);
    }
    void ignoredCompletionCannotReportSuccess_data() {
        completionDoesNotSweepUnrelatedDeletionJobs_data();
    }
    void ignoredCompletionCannotReportSuccess() {
        QFETCH(bool, provider);
        Fixture f;
        QVERIFY(f.pendingDeletion(provider));
        QVERIFY(f.exec("CREATE TRIGGER ignore_completion BEFORE UPDATE ON session_deletion_work "
                       "BEGIN SELECT RAISE(IGNORE); END"));
        QVERIFY(!f.completeDeletion(provider));
        QCOMPARE(f.journal.deletionWork().size(), 1);
        QVERIFY(f.exec("DROP TRIGGER ignore_completion"));
        QVERIFY(f.completeDeletion(provider));
        QVERIFY(f.journal.deletionWork().isEmpty());
    }
    void malformedProviderTargetsCannotBeCompleted_data() {
        QTest::addColumn<QString>("targets");
        QTest::newRow("invalid-json") << QString("broken");
        QTest::newRow("object") << QString("{}");
        QTest::newRow("null") << QString("null");
    }
    void malformedProviderTargetsCannotBeCompleted() {
        QFETCH(QString, targets);
        Fixture f;
        QVERIFY(f.pendingDeletion(true));
        QVERIFY(f.exec("UPDATE session_deletion_work SET provider_targets='" + targets + "'"));
        QVERIFY(!f.completeDeletion(true));
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 1);
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 1);
    }
    void deletionCleanupPreservesEitherPendingGate() {
        Fixture f;
        QVERIFY(f.deletionJobs(3));
        QVERIFY(f.pendingDeletion(true));
        QVERIFY(f.exec("INSERT INTO session_deletion_work "
                       "VALUES('cloud-wait',1,'{\"operationId\":\"wait\"}','[]',0)"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_deletion_work"), 2);
        QCOMPARE(f.journal.deletionWork().size(), 2);
        QCOMPARE(f.journal.deletionWork(64, true).size(), 1);
    }
    void emptyCompletionIdentityCannotFinalizeWork() {
        Fixture f;
        QVERIFY(f.pendingDeletion(true));
        QVERIFY(!f.journal.completeProviderCleanup("target", 1, "", ""));
        QVERIFY(f.exec(
            "UPDATE session_deletion_work SET deletion='{\"operationId\":\"\"}',cloud_acked=0"));
        QVERIFY(!f.journal.acknowledgeCloudDeletion("target", 1, ""));
        QCOMPARE(f.scalar("SELECT cloud_acked FROM session_deletion_work"), 0);
        QCOMPARE(f.journal.deletionWork().size(), 1);
    }
    void indexedDeletionPagesWrapWithoutDuplicates() {
        Fixture f;
        QVERIFY(f.deletionJobs(100000));
        QVERIFY(f.exec(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<6) "
            "INSERT INTO session_deletion_work SELECT "
            "'pending-'||x,1,json_object('operationId','op-'||x),"
            "CASE WHEN x%2=0 THEN '[]' ELSE '[{\"provider\":\"codex\",\"thread_id\":\"t\"}]' END,"
            "CASE WHEN x%2=0 THEN 0 ELSE 1 END FROM n"));
        const auto ids = [](const QJsonArray& rows) {
            QList<qint64> result;
            for (const auto& value : rows)
                result.append(value.toObject().value("work_id").toInteger());
            return result;
        };
        QCOMPARE(ids(f.journal.deletionWork(4, false, 100004)),
                 (QList<qint64>{100005, 100006, 100001, 100002}));
        QCOMPARE(ids(f.journal.deletionWork(3, true, 100004)),
                 (QList<qint64>{100006, 100002, 100004}));
        QCOMPARE(ids(f.journal.deletionWork(256, false, 100003)),
                 (QList<qint64>{100004, 100005, 100006, 100001, 100002, 100003}));
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 25; ++i)
            QCOMPARE(f.journal.deletionWork(64, false, 100003).size(), 6);
        qInfo() << "25 deletion pages over 100000 completed jobs:" << timer.elapsed() << "ms";
    }
    void actualDeletionPagePlansUseStateAndRowidSeeks() {
        Fixture f;
        for (bool cloud : {false, true}) {
            for (bool wrap : {false, true}) {
                QSqlQuery q(f.db);
                QVERIFY(q.prepare("EXPLAIN QUERY PLAN " + detail::deletionWorkQuery(cloud, wrap)));
                q.addBindValue(100000);
                q.addBindValue(64);
                QVERIFY(q.exec());
                QString plan;
                while (q.next())
                    plan += q.value(3).toString() + '\n';
                const QString prefix =
                    cloud ? "session_deletion_cloud_pending (cloud_acked=? AND rowid"
                          : "session_deletion_pending (<expr>=? AND rowid";
                QVERIFY2(plan.contains(prefix + (wrap ? "<?)" : ">?)")), qPrintable(plan));
                QVERIFY2(!plan.contains("TEMP B-TREE"), qPrintable(plan));
            }
        }
    }
    void clockRollbackKeepsTheNewWrite() {
        QFETCH(bool, audit);
        Fixture f;
        QVERIFY(f.seed(audit, 10000));
        for (int i = 0; i < 255; ++i)
            QVERIFY(f.store(audit, QString("new-%1").arg(i)));
        QVERIFY(f.store(audit, "trigger"));
        const QString sql =
            audit ? "SELECT count(*) FROM remote_command_audit WHERE command_id='trigger'"
                  : "SELECT count(*) FROM command_results WHERE message_id='trigger'";
        QCOMPARE(f.scalar(sql), 1);
        if (!audit)
            QVERIFY(f.journal.commandResult("trigger").value("accepted").toBool());
    }
    void onePassDoesNotDeleteTheWholeBacklog_data() { clockRollbackKeepsTheNewWrite_data(); }
    void onePassDoesNotDeleteTheWholeBacklog() {
        QFETCH(bool, audit);
        Fixture f;
        QVERIFY(f.seed(audit, 100000));
        for (int i = 0; i < 255; ++i)
            QVERIFY(f.store(audit, QString("new-%1").arg(i)));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.store(audit, "trigger"));
        const auto elapsed = timer.elapsed();
        const auto remaining = f.scalar(audit ? "SELECT count(*) FROM remote_command_audit"
                                              : "SELECT count(*) FROM command_results");
        const auto removed = 100256 - remaining;
        qInfo() << "Metadata retention audit=" << audit << "removed=" << removed
                << "ms=" << elapsed;
        QVERIFY(removed > 0 && removed <= 256);
        QVERIFY(elapsed < 1000);
    }
    void byteBudgetResumesAfterReopen() {
        Fixture f;
        QVERIFY(f.expiredCommands(5, 400000));
        QCOMPARE(f.scalar("SELECT sum(octet_length(result)) FROM command_results"), 2000000);
        QVERIFY(f.store(false, "fresh"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 4);
        QCOMPARE(
            f.scalar("SELECT age_row FROM journal_metadata_gc WHERE table_name='command_results'"),
            2);
        {
            LocalJournal reopened(f.path);
            QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
            QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 2);
            QCOMPARE(
                f.scalar(
                    "SELECT age_row FROM journal_metadata_gc WHERE table_name='command_results'"),
                4);
            QVERIFY(reopened.pruneRetention());
            QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 1);
            QVERIFY(reopened.commandResult("fresh").value("accepted").toBool());
        }
    }
    void byteBudgetIsSharedByAgeAndCap() {
        Fixture f;
        QVERIFY(f.expiredCommands(1, 600000));
        QVERIFY(f.seed(false, 10001));
        QVERIFY(f.exec(
            "UPDATE command_results SET result=printf('%600000s','x') WHERE message_id='old-1'"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 10001);
        QCOMPARE(
            f.scalar("SELECT cap_row FROM journal_metadata_gc WHERE table_name='command_results'"),
            0);
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 10000);
    }
    void unsupportedRowsDoNotStarveOtherMetadata() {
        Fixture f;
        QVERIFY(f.seed(false, 11000));
        QVERIFY(f.exec("UPDATE command_results SET result=x'01' WHERE rowid<=300"));
        QVERIFY(
            f.exec("UPDATE command_results SET result=printf('%1100000s','x') WHERE rowid=301"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 11000);
        for (int i = 0; i < 8; ++i)
            QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 10301);
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results WHERE rowid<=301"), 301);
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results WHERE rowid>1000"), 10000);
    }
    void orphanAndTombstonePassesPreserveLiveGeneration() {
        Fixture f;
        QVERIFY(f.exec(
            "INSERT INTO "
            "sessions(id,worktree_path,adapter,thread_id,turn_id,state,generation,last_cursor) "
            "VALUES('live','','fake','','','idle',2,0)"));
        const QString prefix =
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<300) ";
        QVERIFY(f.exec(
            prefix +
            "INSERT INTO session_tombstones SELECT 'gone-'||x,1,1,'op','2000-01-01' FROM n"));
        QVERIFY(f.exec(
            prefix +
            "INSERT INTO client_cursors SELECT 'reader','gone-'||x,1,1,'2000-01-01' FROM n"));
        QVERIFY(f.exec(prefix + "INSERT INTO background_tasks SELECT 'gone-'||x,1,'[]' FROM n"));
        QVERIFY(
            f.exec("INSERT INTO client_cursors "
                   "VALUES('reader','live',1,1,'2099-01-01'),('reader','live',2,1,'2099-01-01')"));
        QVERIFY(f.exec("INSERT INTO background_tasks VALUES('live',1,'[]'),('live',2,'[]')"));
        QVERIFY(f.exec("INSERT INTO session_tombstones VALUES('recent',1,1,'op','2099-01-01')"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_tombstones"), 45);
        QCOMPARE(f.scalar("SELECT count(*) FROM client_cursors"), 46);
        QCOMPARE(f.scalar("SELECT count(*) FROM background_tasks"), 46);
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM session_tombstones"), 1);
        QCOMPARE(f.scalar("SELECT count(*) FROM client_cursors"), 1);
        QCOMPARE(f.scalar("SELECT generation FROM client_cursors"), 2);
        QCOMPARE(f.scalar("SELECT count(*) FROM background_tasks"), 1);
        QCOMPARE(f.scalar("SELECT generation FROM background_tasks"), 2);
    }
    void failureRollsBackAgeCapAndState_data() {
        QTest::addColumn<QString>("trigger");
        for (const auto& action : {QString("RAISE(ABORT,'injected')"), QString("RAISE(IGNORE)")}) {
            QTest::newRow(qPrintable("delete-" + action))
                << "BEFORE DELETE ON command_results WHEN old.message_id='old-1' BEGIN SELECT " +
                       action + "; END";
            QTest::newRow(qPrintable("state-" + action))
                << "BEFORE UPDATE ON journal_metadata_gc WHEN old.table_name='command_results' "
                   "BEGIN SELECT " +
                       action + "; END";
        }
    }
    void failureRollsBackAgeCapAndState() {
        QFETCH(QString, trigger);
        Fixture f;
        QVERIFY(f.expiredCommands(1));
        QVERIFY(f.seed(false, 10050));
        QVERIFY(f.exec("CREATE TRIGGER fail_metadata " + trigger));
        QVERIFY(!f.journal.pruneRetention());
        QVERIFY(!f.journal.errorString().isEmpty());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 10051);
        QCOMPARE(f.scalar("SELECT age_row+cap_row FROM journal_metadata_gc WHERE "
                          "table_name='command_results'"),
                 0);
        QVERIFY(f.exec("DROP TRIGGER fail_metadata"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 10000);
    }
    void commitFailureRollsBackRowsAndState() {
        Fixture f;
        QVERIFY(f.expiredCommands(3));
        QVERIFY(f.exec("CREATE TABLE fault_parent(id INTEGER PRIMARY KEY)"));
        QVERIFY(f.exec("CREATE TABLE fault_child(id INTEGER REFERENCES fault_parent(id) DEFERRABLE "
                       "INITIALLY DEFERRED)"));
        QVERIFY(f.exec(
            "CREATE TRIGGER fail_metadata AFTER UPDATE ON journal_metadata_gc "
            "WHEN old.table_name='command_results' BEGIN INSERT INTO fault_child VALUES(1); END"));
        QVERIFY(!f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 3);
        QCOMPARE(f.scalar("SELECT age_row+cap_row FROM journal_metadata_gc WHERE "
                          "table_name='command_results'"),
                 0);
        QCOMPARE(f.scalar("SELECT count(*) FROM fault_child"), 0);
        QVERIFY(f.exec("DROP TRIGGER fail_metadata"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 0);
    }
    void busyWriterDefersAndRestoresTimeout() {
        Fixture f;
        QVERIFY(f.expiredCommands(3));
        QSqlQuery q(f.connection());
        QVERIFY(q.exec("PRAGMA busy_timeout=4321"));
        q.finish();
        QVERIFY(f.exec("BEGIN IMMEDIATE"));
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal.pruneRetention());
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 3);
        QCOMPARE(f.scalar("SELECT sum(age_row+cap_row) FROM journal_metadata_gc"), 0);
        QVERIFY(q.exec("PRAGMA busy_timeout"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 4321);
        q.finish();
        QVERIFY(f.exec("COMMIT"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 0);
    }
    void missingLengthCapabilityDefers() {
        Fixture f;
        QVERIFY(f.expiredCommands(3));
        QSqlQuery q(f.connection());
        QVERIFY(q.exec("CREATE TEMP TABLE pragma_function_list(name TEXT)"));
        q.finish();
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 3);
        QCOMPARE(f.scalar("SELECT sum(age_row+cap_row) FROM journal_metadata_gc"), 0);
        QVERIFY(q.exec("DROP TABLE temp.pragma_function_list"));
        q.finish();
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 0);
    }
    void wrappedCursorRechecksEarlierRows() {
        Fixture f;
        QVERIFY(f.seed(false, 300));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(
            f.scalar("SELECT age_row FROM journal_metadata_gc WHERE table_name='command_results'"),
            256);
        QVERIFY(f.exec("UPDATE command_results SET completed_at='2000-01-01' WHERE rowid=1"));
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 300);
        QVERIFY(f.journal.pruneRetention());
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 299);
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results WHERE rowid=1"), 0);
    }
    void walReaderRetainsSnapshotWithoutBlockingMaintenance() {
        Fixture f;
        QVERIFY(f.expiredCommands(3));
        QVERIFY(f.exec("BEGIN"));
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 3);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(f.journal.pruneRetention());
        QVERIFY(timer.elapsed() < 500);
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 3);
        QVERIFY(f.exec("COMMIT"));
        QCOMPARE(f.scalar("SELECT count(*) FROM command_results"), 0);
    }
    void actualPreparedPlansBoundCandidateAndParentLookups() {
        Fixture f;
        for (const auto& name : {QString("command_results"), QString("remote_command_audit"),
                                 QString("session_tombstones"), QString("client_cursors"),
                                 QString("background_tasks"), QString("session_deletion_work")}) {
            QSqlQuery q(f.db);
            QVERIFY(q.prepare("EXPLAIN QUERY PLAN " + detail::metadataRetentionScanQuery(name)));
            q.addBindValue(0);
            q.addBindValue(100000);
            q.addBindValue(256);
            QVERIFY(q.exec());
            QString plan;
            while (q.next())
                plan += q.value(3).toString() + '\n';
            QVERIFY2(plan.contains("SEARCH t USING INTEGER PRIMARY KEY (rowid>? AND rowid<?)"),
                     qPrintable(plan));
            QVERIFY2(!plan.contains("TEMP B-TREE"), qPrintable(plan));
            if (name == "client_cursors" || name == "background_tasks")
                QVERIFY2(plan.contains("SEARCH s USING INDEX sqlite_autoindex_sessions_1 (id=?)"),
                         qPrintable(plan));
        }
        for (const auto& name : {QString("command_results"), QString("remote_command_audit")}) {
            QSqlQuery q(f.db);
            QVERIFY(q.exec("EXPLAIN QUERY PLAN " + detail::metadataRetentionCutoffQuery(name)));
            while (q.next())
                QVERIFY2(!q.value(3).toString().contains("TEMP B-TREE"),
                         qPrintable(q.value(3).toString()));
        }
    }
};
QTEST_GUILESS_MAIN(JournalMetadataRetentionTest)
#include "JournalMetadataRetentionTest.moc"
