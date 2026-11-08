#include "storage/LocalJournal.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
QSqlDatabase journalConnection(const QString& path) {
    for (const auto& name : QSqlDatabase::connectionNames())
        if (name.startsWith("ketplus-daemon-") &&
            QSqlDatabase::database(name).databaseName() == path)
            return QSqlDatabase::database(name);
    return {};
}

QVariantList activityRows(QSqlDatabase db, const QString& session) {
    QVariantList rows;
    for (const auto& table : {"activity_items", "activity_summaries", "activity_heads"}) {
        QSqlQuery q(db);
        q.prepare(QString("SELECT * FROM %1 WHERE session_id=? ORDER BY 1,2,3,4").arg(table));
        q.addBindValue(session);
        if (!q.exec())
            return {};
        while (q.next())
            for (int i = 1; i < q.record().count(); ++i)
                rows.append(q.value(i));
    }
    return rows;
}

// Two turns with tool items, so grouping depends on the order history is indexed in.
void writeActivity(LocalJournal& journal, const QString& session) {
    for (int turn = 1; turn <= 2; ++turn) {
        journal.appendEvent(session, 1, "user.message", {{"text", "go"}});
        for (int item = 0; item < 3; ++item) {
            const QJsonObject tool{{"id", QString("t%1-%2").arg(turn).arg(item)},
                                   {"type", "commandExecution"}};
            journal.appendEvent(session, 1, "item.started",
                                {{"item_id", tool.value("id")}, {"item", tool}});
            journal.appendEvent(session, 1, "item.completed",
                                {{"item_id", tool.value("id")}, {"item", tool}});
        }
        journal.appendEvent(session, 1, "turn.completed", {{"status", "completed"}});
    }
}

void forgetActivity(QSqlDatabase db, const QString& session) {
    QSqlQuery q(db);
    for (const auto& table : {"activity_items", "activity_summaries", "activity_heads"}) {
        q.prepare(QString("DELETE FROM %1 WHERE session_id=?").arg(table));
        q.addBindValue(session);
        q.exec();
    }
}
} // namespace

class JournalStartupTest final : public QObject {
    Q_OBJECT
  private slots:
    void legacyBackfillsRunOnlyBeforeTheFormatMarker_data() {
        QTest::addColumn<int>("format");
        QTest::newRow("pre-versioning journal") << 0;
        QTest::newRow("stamped journal") << 1;
    }
    void legacyBackfillsRunOnlyBeforeTheFormatMarker() {
        // An image-only first prompt leaves an empty title: before ADR 0096 every start
        // rescanned that conversation's whole history to backfill it again.
        QFETCH(int, format);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            StoredSession session{"image-only", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
            QVERIFY(journal.upsertSession(session));
            QVERIFY(journal.appendEvent("image-only", 1, "user.message", {{"text", ""}}) > 0);
            QVERIFY(journal.appendEvent("image-only", 1, "assistant.delta", {{"text", "x"}}) > 0);
        }
        const QString connection = QStringLiteral("legacy-backfill-test");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("UPDATE events SET turn_sequence=0"));
            QVERIFY(q.exec("UPDATE sessions SET title='', last_turn_started_at=''"));
            QVERIFY(q.exec(QString("PRAGMA user_version=%1").arg(format)));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        qint64 numbered = 0;
        for (const auto& value : reopened.eventsAfter("image-only", 1, 0))
            numbered += value.toObject().value("turn_sequence").toInteger() > 0 ? 1 : 0;
        QString startedAt;
        for (const auto& session : reopened.sessions())
            if (session.id == "image-only")
                startedAt = session.lastTurnStartedAt;
        if (format == 0) {
            QCOMPARE(numbered, qint64(2));
            QVERIFY(!startedAt.isEmpty());
        } else {
            QCOMPARE(numbered, qint64(0));
            QVERIFY(startedAt.isEmpty());
        }
    }
    void activityIsIndexedAfterOpenNotDuringIt() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        QVariantList indexed;
        {
            LocalJournal journal(path);
            QVERIFY(journal.upsertSession({"s", dir.path(), "codex", {}, {}, "ready"}));
            writeActivity(journal, "s");
            indexed = activityRows(journalConnection(path), "s");
            QVERIFY(!indexed.isEmpty());
            // A journal from before the activity index has none of it.
            forgetActivity(journalConnection(path), "s");
            QSqlQuery q(journalConnection(path));
            QVERIFY(q.exec("CREATE TRIGGER fail_activity AFTER INSERT ON activity_items BEGIN "
                           "SELECT RAISE(ABORT,'activity fault'); END"));
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QVERIFY(reopened.hasPendingActivityBackfill());
        // A failed pass rolls back, releases its transaction and keeps the marker.
        QCOMPARE(reopened.backfillActivityGroups(), LocalJournal::ActivityBackfill::Failed);
        QSqlQuery q(journalConnection(path));
        QVERIFY2(q.exec("BEGIN IMMEDIATE"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QVERIFY(activityRows(journalConnection(path), "s").isEmpty());
        QVERIFY(reopened.hasPendingActivityBackfill());
        QVERIFY(q.exec("DROP TRIGGER fail_activity"));
        while (reopened.backfillActivityGroups() == LocalJournal::ActivityBackfill::Progress) {
        }
        QVERIFY(!reopened.hasPendingActivityBackfill());
        QCOMPARE(activityRows(journalConnection(path), "s"), indexed);
    }
    void aNewEventIndexesItsSessionsHistoryFirst() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        QVariantList expected;
        {
            // Reference: the same history indexed live, then one more tool item.
            LocalJournal journal(dir.filePath("reference.sqlite"));
            QVERIFY(journal.upsertSession({"s", dir.path(), "codex", {}, {}, "ready"}));
            writeActivity(journal, "s");
            journal.appendEvent("s", 1, "user.message", {{"text", "more"}});
            const QJsonObject tool{{"id", "late"}, {"type", "commandExecution"}};
            journal.appendEvent("s", 1, "item.started", {{"item_id", "late"}, {"item", tool}});
            expected = activityRows(journalConnection(dir.filePath("reference.sqlite")), "s");
        }
        {
            LocalJournal journal(path);
            QVERIFY(journal.upsertSession({"s", dir.path(), "codex", {}, {}, "ready"}));
            writeActivity(journal, "s");
            forgetActivity(journalConnection(path), "s");
        }
        LocalJournal reopened(path);
        QVERIFY(reopened.hasPendingActivityBackfill());
        // No background pass ran: the append itself catches up before indexing its event.
        reopened.appendEvent("s", 1, "user.message", {{"text", "more"}});
        QVERIFY(!reopened.hasPendingActivityBackfill());
        const QJsonObject tool{{"id", "late"}, {"type", "commandExecution"}};
        reopened.appendEvent("s", 1, "item.started", {{"item_id", "late"}, {"item", tool}});
        QCOMPARE(activityRows(journalConnection(path), "s"), expected);
        // A fork of a session still being indexed keeps its own marker.
        {
            LocalJournal journal(dir.filePath("fork.sqlite"));
            QVERIFY(journal.upsertSession({"s", dir.path(), "codex", {}, {}, "ready"}));
            writeActivity(journal, "s");
            forgetActivity(journalConnection(dir.filePath("fork.sqlite")), "s");
        }
        LocalJournal forking(dir.filePath("fork.sqlite"));
        StoredSession copy{"copy", dir.path(), "codex", {}, {}, "ready"};
        QString error;
        QVERIFY2(forking.forkSession("s", 1, copy, &error), qPrintable(error));
        QSqlQuery q(journalConnection(dir.filePath("fork.sqlite")));
        QVERIFY(q.exec("SELECT session_id FROM activity_backfill ORDER BY 1"));
        QStringList marked;
        while (q.next())
            marked.append(q.value(0).toString());
        QCOMPARE(marked, QStringList({"copy", "s"}));
    }
    void startupBacklogCharacterization_data() {
        QTest::addColumn<int>("rows");
        QTest::addColumn<int>("sessions");
        QTest::newRow("1000") << 1000 << 100;
        QTest::newRow("300000") << 300000 << 20000;
    }
    void startupBacklogCharacterization() {
        QFETCH(int, rows);
        QFETCH(int, sessions);
        if (rows > 1000 && !qEnvironmentVariableIsSet("KETPLUS_RUN_STARTUP_BACKLOG_TESTS"))
            QSKIP("Opt-in synthetic backlog characterization");
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            QSqlDatabase db;
            for (const auto& name : QSqlDatabase::connectionNames())
                if (name.startsWith("ketplus-daemon-") &&
                    QSqlDatabase::database(name).databaseName() == path)
                    db = QSqlDatabase::database(name);
            QVERIFY(db.isOpen());
            QSqlQuery q(db);
            QVERIFY(db.transaction());
            const auto n = [](int count) {
                return QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                               "WHERE i<%1) ")
                    .arg(count);
            };
            QVERIFY2(q.exec(n(sessions) +
                            "INSERT INTO sessions(id,worktree_path,adapter,thread_id,turn_id,state,"
                            "generation,last_cursor,current_turn_sequence) SELECT 's'||i,'/tmp',"
                            "'codex','','','ready',1,3,1 FROM n"),
                     qPrintable(q.lastError().text()));
            QVERIFY2(q.exec(n(sessions) + "INSERT INTO "
                                          "replay_projection_state(session_id,generation,first_"
                                          "cursor,last_cursor) SELECT 's'||i,1,1,3 FROM n"),
                     qPrintable(q.lastError().text()));
            const QString spread = QString("'s'||(1+i%%1)").arg(sessions);
            QVERIFY2(q.exec(n(rows) +
                            "INSERT INTO events(session_id,generation,cursor,event_type,"
                            "payload,sent_at,turn_sequence) SELECT " +
                            spread +
                            ",1,i,'assistant.delta','{\"text\":\"x\"}',datetime('now'),1 "
                            "FROM n"),
                     qPrintable(q.lastError().text()));
            QVERIFY2(q.exec(n(rows) +
                            "INSERT INTO replay_events(session_id,generation,cursor,"
                            "event_type,item_id,payload,sent_at,turn_sequence) SELECT " +
                            spread +
                            ",1,i,'assistant.delta','item','{\"text\":\"x\"}',datetime('now'),"
                            "1 FROM n"),
                     qPrintable(q.lastError().text()));
            QVERIFY(db.commit());
            QVERIFY(q.exec("PRAGMA wal_checkpoint(TRUNCATE)"));
        }
        QElapsedTimer timer;
        timer.start();
        LocalJournal reopened(path);
        const auto elapsed = timer.elapsed();
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        qInfo() << "startup-characterization rows" << rows << "sessions" << sessions << "open-ms"
                << elapsed;
        // The one-time activity index no longer blocks opening (ADR 0097).
        QVERIFY2(elapsed < 250, qPrintable(QString::number(elapsed)));
        qint64 slowestPass = 0;
        int passes = 0;
        for (;;) {
            QElapsedTimer pass;
            pass.start();
            const auto step = reopened.backfillActivityGroups();
            slowestPass = qMax(slowestPass, pass.elapsed());
            if (step == LocalJournal::ActivityBackfill::Idle)
                break;
            QCOMPARE(step, LocalJournal::ActivityBackfill::Progress);
            QVERIFY(++passes < 1000000);
        }
        qInfo() << "startup-characterization backfill-passes" << passes << "slowest-pass-ms"
                << slowestPass;
        QVERIFY2(slowestPass < 250, qPrintable(QString::number(slowestPass)));
        QElapsedTimer again;
        again.start();
        LocalJournal second(path);
        QVERIFY2(second.isOpen(), qPrintable(second.errorString()));
        const auto steady = again.elapsed();
        qInfo() << "startup-characterization second-open-ms" << steady;
        // After one-time backfills, opening must not rescan history (ADR 0096).
        QVERIFY2(steady < 250, qPrintable(QString::number(steady)));
    }
};

QTEST_GUILESS_MAIN(JournalStartupTest)
#include "JournalStartupTest.moc"
