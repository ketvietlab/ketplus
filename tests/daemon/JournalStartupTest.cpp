#include "storage/LocalJournal.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

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
