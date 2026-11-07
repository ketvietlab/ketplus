#include "storage/LocalJournal.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <thread>

using namespace ketplus::daemon;

namespace {
class Database {
  public:
    explicit Database(const QString& path) : name_(QUuid::createUuid().toString()) {
        db_ = QSqlDatabase::addDatabase("QSQLITE", name_);
        db_.setDatabaseName(path);
        if (!db_.open())
            qFatal("Fixture database cannot open");
    }
    ~Database() {
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool exec(const QString& statement) {
        QSqlQuery query(db_);
        if (query.exec(statement))
            return true;
        qWarning() << query.lastError();
        return false;
    }
    QString queryPlan(const QString& statement) {
        QSqlQuery query(db_);
        if (!query.exec("EXPLAIN QUERY PLAN " + statement))
            return query.lastError().text();
        QStringList details;
        while (query.next())
            details.append(query.value(3).toString());
        return details.join('\n');
    }
    qint64 scalar(const QString& statement) {
        QSqlQuery query(db_);
        if (!query.exec(statement) || !query.next())
            return -1;
        return query.value(0).toLongLong();
    }

  private:
    QString name_;
    QSqlDatabase db_;
};

QDateTime cutoff() { return QDateTime::currentDateTimeUtc().addDays(-30); }

bool seed(LocalJournal& journal, const QString& path, bool acknowledge = true, int turns = 24,
          const QString& output = {}) {
    StoredSession session;
    session.id = "session";
    session.worktreePath = "synthetic";
    session.adapter = "codex";
    session.state = "ready";
    if (!journal.upsertSession(session))
        return false;
    qint64 cursor = 0;
    const auto append = [&](const QString& type, const QJsonObject& data) {
        cursor = journal.appendEvent("session", 1, type, data);
        return cursor > 0;
    };
    for (int turn = 1; turn <= turns; ++turn) {
        if (!append("user.message", {{"text", QString("prompt-%1").arg(turn)}}) ||
            !append("turn.started", {}))
            return false;
        for (const QString& type :
             {QString("usage.context"), QString("usage.limits"), QString("usage.session")}) {
            for (int sample = 1; sample <= 3; ++sample)
                if (!append(type, {{"sample", sample}, {"turn", turn}}))
                    return false;
        }
        QJsonObject item{
            {"id", QString("item-%1").arg(turn)}, {"type", "agentMessage"}, {"text", "answer"}};
        if (!output.isEmpty()) {
            item = {{"id", QString("item-%1").arg(turn)},
                    {"type", "commandExecution"},
                    {"command", "synthetic-command"},
                    {"cwd", "synthetic"},
                    {"exitCode", 0},
                    {"durationMs", 123},
                    {"status", "completed"},
                    {"aggregatedOutput", output}};
        }
        if (!append("item.started", {{"item", item}}) ||
            !append("item.completed", {{"item", item}}) || !append("turn.completed", {}))
            return false;
    }
    Database database(path);
    return database.exec("UPDATE replay_events SET sent_at='2020-01-01T00:00:00.000Z'") &&
           (!acknowledge || journal.acknowledgeCursor("client", "session", 1, cursor));
}

qint64 sweep(LocalJournal& journal, int maximumRows = 128) {
    qint64 total = 0;
    // The fixture has at most 336 rows. A few complete sweeps also prove idempotency.
    for (int pass = 0; pass < 8; ++pass) {
        const auto removed = journal.pruneReplayDetails(cutoff(), maximumRows);
        if (!removed)
            return -1;
        total += *removed;
    }
    return total;
}

QJsonArray allReplay(LocalJournal& journal) {
    QJsonArray result;
    qint64 cursor = 0;
    for (;;) {
        const auto page = journal.compactedEventsAfter("session", 1, cursor, 500);
        if (page.events.isEmpty())
            return result;
        if (page.nextCursor <= cursor)
            qFatal("Replay cursor made no progress");
        for (const auto& event : page.events)
            result.append(event);
        cursor = page.nextCursor;
    }
}

QJsonArray withoutUsage(const QJsonArray& events) {
    QJsonArray result;
    for (const auto& event : events) {
        if (!event.toObject().value("event_type").toString().startsWith("usage."))
            result.append(event);
    }
    return result;
}
} // namespace

class JournalUsageRetentionTest final : public QObject {
    Q_OBJECT
  private slots:
    void retainsLastSamplesMessagesOrderingAndFencesContinuations() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        const auto before = journal.compactedEventsAfter("session", 1, 0, 500);
        const auto window = journal.transcriptWindow("session", 1);
        QVERIFY(window.hasMoreBefore);
        QCOMPARE(sweep(journal), 24); // 4 old turns * 3 types * 2 intermediate samples
        const auto after = journal.compactedEventsAfter("session", 1, 0, 500);
        QCOMPARE(withoutUsage(after.events), withoutUsage(before.events));
        QCOMPARE(after.nextCursor, before.nextCursor);
        auto page = journal.transcriptPage("session", 1, 0, 20);
        const auto older = journal.transcriptPage("session", 1, page.earliestTurnSequence, 20);
        auto paged = older.events;
        for (const auto& event : page.events)
            paged.append(event);
        QCOMPARE(paged, after.events);
        Database database(path);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence<=4 AND "
                                 "event_type LIKE 'usage.%'"),
                 12);
        QCOMPARE(
            database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence<=4 AND "
                            "event_type LIKE 'usage.%' AND json_extract(payload,'$.sample')!=3"),
            0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence>4 AND "
                                 "event_type LIKE 'usage.%'"),
                 180);
        QCOMPARE(journal
                     .transcriptWindow("session", 1, window.beforeCursor, window.snapshotCursor,
                                       window.projectionRevision)
                     .errorCode,
                 QString("snapshot_changed"));
        const auto revision = journal.transcriptWindow("session", 1).projectionRevision;
        QCOMPARE(sweep(journal), 0);
        QCOMPARE(journal.transcriptWindow("session", 1).projectionRevision, revision);
    }

    void protectsTurns_data() {
        QTest::addColumn<QString>("guard");
        QTest::addColumn<bool>("commands");
        for (const char* guard :
             {"running", "waiting_for_input", "starting", "stopping", "unacknowledged", "recent",
              "open", "approval", "question", "generation", "sequence-gap"})
            for (bool commands : {false, true})
                QTest::newRow(qPrintable(QString(guard) + (commands ? "-commands" : "-usage")))
                    << QString(guard) << commands;
    }

    void protectsTurns() {
        QFETCH(QString, guard);
        QFETCH(bool, commands);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacknowledged", 24,
                     commands ? QString(50000, QLatin1Char('x')) : QString()));
        Database database(path);
        if (guard == "unacknowledged") {
            QVERIFY(journal.acknowledgeCursor("slow", "session", 1, 0));
            const auto raw = journal.eventsAfter("session", 1, 0, 500);
            QVERIFY(journal.acknowledgeCursor("fast", "session", 1,
                                              raw.last().toObject().value("cursor").toInteger()));
        }
        if (guard == "running" || guard == "waiting_for_input" || guard == "starting" ||
            guard == "stopping")
            QVERIFY(database.exec(QString("UPDATE sessions SET state='%1'").arg(guard)));
        else if (guard == "recent")
            QVERIFY(database.exec(
                "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE "
                "event_type='turn.completed'"));
        else if (guard == "open")
            QVERIFY(database.exec("DELETE FROM replay_events WHERE event_type='turn.completed'"));
        else if (guard == "approval" || guard == "question")
            QVERIFY(database.exec(QString("UPDATE replay_events SET event_type='%1.requested' "
                                          "WHERE event_type='item.started'")
                                      .arg(guard)));
        else if (guard == "generation")
            QVERIFY(database.exec("UPDATE sessions SET generation=2"));
        else if (guard == "sequence-gap") {
            // Only 19 actual newer turns, despite a large numeric sequence gap.
            QVERIFY(database.exec("DELETE FROM replay_events WHERE turn_sequence BETWEEN 2 AND 5"));
            QVERIFY(database.exec("UPDATE replay_events SET turn_sequence=turn_sequence*100"));
            QVERIFY(database.exec("UPDATE sessions SET current_turn_sequence=2400"));
        }
        const auto before = journal.compactedEventsAfter("session", 1, 0, 500).events;
        QCOMPARE(sweep(journal), 0);
        QCOMPARE(journal.compactedEventsAfter("session", 1, 0, 500).events, before);
        if (guard == "unacknowledged") {
            const auto raw = journal.eventsAfter("session", 1, 0, 500);
            QCOMPARE(raw.size(), before.size());
            QVERIFY(journal.acknowledgeCursor("slow", "session", 1,
                                              raw.last().toObject().value("cursor").toInteger()));
            QCOMPARE(sweep(journal), commands ? 28 : 24); // ineligible rows are revisited
        }
    }

    void boundsTheScanAndMakesProgressPastUninterestingRows() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        // The first two rows are user.message and turn.started: no deletion yet.
        QCOMPARE(journal.pruneReplayDetails(cutoff(), 1), std::optional<qint64>(0));
        QCOMPARE(journal.pruneReplayDetails(cutoff(), 1), std::optional<qint64>(0));
        QCOMPARE(journal.pruneReplayDetails(cutoff(), 1), std::optional<qint64>(1));
        qint64 removed = 1;
        for (int pass = 0; pass < 700; ++pass) {
            const auto result = journal.pruneReplayDetails(cutoff(), 1);
            QVERIFY(result.has_value());
            QVERIFY(*result <= 1);
            removed += *result;
        }
        QCOMPARE(removed, 24);
    }

    void rollsBackDeletesRevisionAndScanProgressTogether() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        Database database(path);
        const auto before = journal.compactedEventsAfter("session", 1, 0, 500).events;
        const auto revision = journal.transcriptWindow("session", 1).projectionRevision;
        QVERIFY(database.exec(
            "CREATE TRIGGER fail_retention BEFORE UPDATE OF revision ON replay_projection_state "
            "BEGIN SELECT RAISE(ABORT, 'synthetic revision failure'); END"));
        QVERIFY(!journal.pruneReplayDetails(cutoff()).has_value());
        QVERIFY(journal.errorString().contains("synthetic revision failure"));
        QCOMPARE(journal.compactedEventsAfter("session", 1, 0, 500).events, before);
        QCOMPARE(journal.transcriptWindow("session", 1).projectionRevision, revision);
        QVERIFY(database.exec("DROP TRIGGER fail_retention"));
        QCOMPARE(journal.pruneReplayDetails(cutoff()), std::optional<qint64>(24));
        QCOMPARE(sweep(journal), 0);
    }

    void yieldsToAnotherWriterAndKeepsWorkingAfterward() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        QSemaphore locked;
        QSemaphore release;
        bool writerReady = false;
        std::thread writer([&] {
            Database database(path);
            writerReady = database.exec("BEGIN IMMEDIATE");
            locked.release();
            release.acquire();
            QThread::msleep(200);
            database.exec("ROLLBACK");
        });
        locked.acquire();
        QElapsedTimer timer;
        timer.start();
        const auto maintenance = journal.pruneReplayDetails(cutoff());
        const auto elapsed = timer.elapsed();
        release.release();
        // The normal connection timeout must be restored: this ordinary write waits for
        // the writer's delayed release and succeeds, instead of failing with SQLITE_BUSY.
        const bool written = journal.markSessionSeen("session", 1, 1);
        writer.join();
        QVERIFY(writerReady);
        QVERIFY(!maintenance.has_value());
        QVERIFY(elapsed < 1000);
        QVERIFY(written);
        QCOMPARE(sweep(journal), 24);
    }

    void rawBacklogUsesTurnLookup_data() {
        QTest::addColumn<bool>("upgrade");
        QTest::newRow("fresh") << false;
        QTest::newRow("existing-journal") << true;
    }

    void rawBacklogUsesTurnLookup() {
        QFETCH(bool, upgrade);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, path));
        }
        Database database(path);
        // A later unacknowledged turn must not make each older-turn ACK probe scan
        // the entire raw backlog. This is synthetic storage, never a provider call.
        QVERIFY(database.exec(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<300000) "
            "INSERT INTO "
            "events(session_id,generation,cursor,event_type,payload,sent_at,turn_sequence) "
            "SELECT 'session',1,1000+x,'usage.context','{}','2020-01-01T00:00:00.000Z',24 FROM n"));
        if (upgrade)
            QVERIFY(database.exec("DROP INDEX IF EXISTS events_turn"));
        LocalJournal journal(path); // also installs the index on pre-existing journals
        Database migrated(path);    // fresh reader sees the migrated schema, not a cached plan
        const auto plan = migrated.queryPlan(
            "SELECT 1 FROM events WHERE session_id='session' AND generation=1 AND turn_sequence=1");
        QElapsedTimer timer;
        timer.start();
        const auto removed = journal.pruneReplayDetails(cutoff(), 128);
        const auto elapsed = timer.elapsed();
        qInfo().noquote() << "300000 raw events, 128 replay rows:" << elapsed << "ms;" << plan;
        QCOMPARE(removed, std::optional<qint64>(24));
        QCOMPARE(database.scalar("SELECT count(*) FROM events"), 300000);
        QVERIFY2(plan.contains("COVERING INDEX events_turn") &&
                     plan.contains("session_id=? AND generation=? AND turn_sequence=?"),
                 qPrintable(plan));
        // Wide headroom for slow runners; query-plan assertion is the deterministic guard.
        QVERIFY2(elapsed < 1000, "Idle retention stalled on unrelated raw backlog");
    }

    void commandOutputRetainsMetadataUtf8AndExplicitNotice() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        const QString output = QStringLiteral("HEAD\n") + QString(20000, QChar(0x754c)) + "\n" +
                               QStringLiteral("tail-line\n").repeated(100) + "TAIL";
        QVERIFY(seed(journal, path, true, 24, output));
        const auto before = allReplay(journal);
        const auto window = journal.transcriptWindow("session", 1);
        QCOMPARE(sweep(journal), 28);
        const auto after = allReplay(journal);
        auto expected = withoutUsage(before);
        const auto actual = withoutUsage(after);
        QCOMPARE(actual.size(), expected.size());
        int trimmed = 0;
        for (qsizetype i = 0; i < actual.size(); ++i) {
            auto event = actual.at(i).toObject();
            auto payload = event.value("payload").toObject();
            // Replay envelopes store provider data under payload.
            auto item = payload.value("item").toObject();
            if (!item.contains("compacted")) {
                QCOMPARE(event, expected.at(i).toObject());
                continue;
            }
            ++trimmed;
            const auto marker = item.value("compacted").toObject();
            QCOMPARE(marker.value("original_bytes").toInteger(), output.toUtf8().size());
            QVERIFY(marker.value("retained_bytes").toInteger() <= 20 * 1024);
            const QString retained = item.value("aggregatedOutput").toString();
            QVERIFY(retained.startsWith("[Output trimmed"));
            QVERIFY(retained.contains("HEAD\n"));
            QVERIFY(retained.endsWith("\nTAIL"));
            QVERIFY(!retained.contains(QChar::ReplacementCharacter));
            item.remove("compacted");
            item.insert("aggregatedOutput", output);
            payload.insert("item", item);
            event.insert("payload", payload);
            QCOMPARE(event, expected.at(i).toObject());
        }
        QCOMPARE(trimmed, 4);
        QCOMPARE(after.last().toObject().value("cursor"), before.last().toObject().value("cursor"));
        QCOMPARE(journal
                     .transcriptWindow("session", 1, window.beforeCursor, window.snapshotCursor,
                                       window.projectionRevision)
                     .errorCode,
                 QString("snapshot_changed"));
        QCOMPARE(sweep(journal), 0);
        const auto receipt = journal.commandOutput("session", 1, 1, "item-1");
        QVERIFY(receipt.value("ok").toBool());
        QVERIFY(receipt.value("truncated").toBool());
        QVERIFY(receipt.value("text").toString().startsWith("[Output trimmed"));
        QVERIFY(receipt.value("text").toString().split('\n').size() <= 40);
        QCOMPARE(receipt.value("compacted").toObject().value("original_bytes").toInteger(),
                 output.toUtf8().size());
    }

    void commandBudgetResumesAndRollbackRestoresPayloads() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 30, QString(230000, QLatin1Char('x'))));
        Database database(path);
        const auto before = allReplay(journal);
        QVERIFY(database.exec(
            "CREATE TRIGGER fail_detail BEFORE UPDATE OF revision ON replay_projection_state "
            "BEGIN SELECT RAISE(ABORT, 'synthetic detail failure'); END"));
        QVERIFY(!journal.pruneReplayDetails(cutoff()).has_value());
        QCOMPARE(allReplay(journal), before);
        QVERIFY(database.exec("DROP TRIGGER fail_detail"));
        QElapsedTimer passTimer;
        passTimer.start();
        QVERIFY(journal.pruneReplayDetails(cutoff()).has_value());
        qInfo() << "Warm command retention pass, 230000-byte outputs:" << passTimer.elapsed()
                << "ms";
        const auto first = database.scalar("SELECT count(*) FROM replay_events WHERE "
                                           "json_type(payload,'$.item.compacted')='object'");
        QVERIFY(first > 0 && first <= 4);
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 10);
        QCOMPARE(sweep(journal), 0);
    }

    void preservesUnsupportedCommandPayloads_data() {
        QTest::addColumn<QString>("replacement");
        QTest::newRow("small") << QString("json_set(payload,'$.item.aggregatedOutput','small')");
        QTest::newRow("near-limit")
            << QString("json_set(payload,'$.item.aggregatedOutput',printf('%.*c',21504,'x'))");
        QTest::newRow("nonterminal") << QString("json_set(payload,'$.item.status','inProgress')");
        QTest::newRow("marked") << QString("json_set(payload,'$.item.compacted',json('{}'))");
        QTest::newRow("oversize") << QString(
            "json_set(payload,'$.item.aggregatedOutput',printf('%.*c',1100000,'x'))");
    }

    void preservesUnsupportedCommandPayloads() {
        QFETCH(QString, replacement);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 24, QString(50000, QLatin1Char('x'))));
        Database database(path);
        QVERIFY(database.exec("UPDATE replay_events SET payload=" + replacement +
                              " WHERE event_type='item.completed'"));
        const auto before = withoutUsage(allReplay(journal));
        QCOMPARE(sweep(journal), 24); // only intermediate usage is changed
        QCOMPARE(withoutUsage(allReplay(journal)), before);
    }

    void invalidLimitsDoNotTouchHistory() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        for (int rows : {-1, 0, 257})
            QVERIFY(!journal.pruneReplayDetails(cutoff(), rows).has_value());
        QVERIFY(!journal.pruneReplayDetails({}).has_value());
        QVERIFY(
            !journal.pruneReplayDetails(QDateTime::currentDateTimeUtc().addDays(1)).has_value());
        QCOMPARE(sweep(journal), 24);
    }
};

QTEST_GUILESS_MAIN(JournalUsageRetentionTest)
#include "JournalUsageRetentionTest.moc"
