#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <sqlite3.h>

#include <functional>
#include <utility>

using namespace ketplus::daemon;

// Records every statement the journal's own connection runs during one operation, with
// SQLite's own counters, on a journal whose other sessions hold far more history than the
// one being used. An operation on one conversation must not walk the others' history.
namespace {
constexpr int backgroundSessions = 2000;
constexpr int backgroundRows = 200000;
constexpr int tombstones = 20000;
// History of the conversation in use, so reads must also stay bounded inside one session.
constexpr int hotBacklogRows = 100000;
// A walk over a few percent of the history is a scan, not a seek.
constexpr int maximumFullScanSteps = 5000;
constexpr int maximumVmSteps = 200000;

struct Statement {
    QString sql;
    bool otherConnection{false};
    int fullScanSteps{0};
    int autoIndexes{0};
    int vmSteps{0};
};

// Statements of the operation being audited, on any connection; null between operations.
QList<Statement>* recording = nullptr;
sqlite3* mainConnection = nullptr;

int recordStatement(unsigned type, void*, void* statement, void*) {
    auto* stmt = static_cast<sqlite3_stmt*>(statement);
    const int fullScan = sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
    const int autoIndexes = sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_AUTOINDEX, 1);
    const int vmSteps = sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_VM_STEP, 1);
    if (type == SQLITE_TRACE_PROFILE && recording)
        recording->append({QString::fromUtf8(sqlite3_sql(stmt)).simplified(),
                           sqlite3_db_handle(stmt) != mainConnection, fullScan, autoIndexes,
                           vmSteps});
    return 0;
}

sqlite3* sqliteHandle(const QSqlDatabase& db) {
    return *static_cast<sqlite3* const*>(db.driver()->handle().data());
}

QSqlDatabase journalConnection(const QString& path) {
    for (const auto& name : QSqlDatabase::connectionNames())
        if (name.startsWith("ketplus-daemon-") &&
            QSqlDatabase::database(name).databaseName() == path)
            return QSqlDatabase::database(name);
    return {};
}

QString sqlRange(int count) {
    return QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<%1) ")
        .arg(count);
}
} // namespace

class JournalQueryAuditTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        path_ = dir_.filePath("journal.sqlite");
        journal_ = std::make_unique<LocalJournal>(path_);
        QVERIFY(journal_->isOpen());
        db_ = journalConnection(path_);
        QVERIFY(db_.isOpen());
        const QVariant handle = db_.driver()->handle();
        if (qstrcmp(handle.typeName(), "sqlite3*") != 0)
            QSKIP("The SQL driver does not expose its sqlite3 handle");
        QSqlQuery source(db_);
        QVERIFY(source.exec("SELECT sqlite_source_id()") && source.next());
        // Calling into the handle is only safe with the same SQLite the driver uses.
        if (source.value(0).toString() != QString::fromUtf8(sqlite3_sourceid()))
            QSKIP("The SQL driver bundles a different SQLite build");
        mainConnection = sqliteHandle(db_);
        sqlite3_trace_v2(mainConnection, SQLITE_TRACE_PROFILE, recordStatement, nullptr);
        traced_ = true;
        writeBackground();
        writeHotSession();
    }

    void cleanupTestCase() {
        if (traced_)
            sqlite3_trace_v2(mainConnection, 0, nullptr, nullptr);
        db_ = {};
        journal_.reset();
    }

    void operationsOnOneSessionStayInsideIt_data() {
        QTest::addColumn<QString>("operation");
        for (const auto& name : {"append",
                                 "turn-start",
                                 "events-after",
                                 "compacted-events-after",
                                 "transcript-page",
                                 "transcript-window",
                                 "transcript-window-after",
                                 "activity-summary",
                                 "activity-group-page",
                                 "command-output",
                                 "session-head",
                                 "open-turn",
                                 "mark-seen",
                                 "acknowledge",
                                 "sessions",
                                 "deletion-work",
                                 "tombstone-page",
                                 "payload-garbage",
                                 "large-event-fragment",
                                 "fork",
                                 "delete-and-purge",
                                 "activity-backfill",
                                 "activity-files-backfill",
                                 "retention",
                                 "acknowledged-events",
                                 "disk-usage"})
            QTest::newRow(name) << QString(name);
    }

    void operationsOnOneSessionStayInsideIt() {
        QFETCH(QString, operation);
        const auto run = operations().value(operation);
        QVERIFY(run);
        QList<Statement> statements;
        recording = &statements;
        run();
        recording = nullptr;
        QStringList violations;
        int vmSteps = 0;
        for (const auto& statement : std::as_const(statements)) {
            vmSteps += statement.vmSteps;
            if (statement.fullScanSteps > maximumFullScanSteps || statement.autoIndexes > 0 ||
                statement.vmSteps > maximumVmSteps)
                violations.append(QString("%1scan %2 autoindex %3 vm %4: %5")
                                      .arg(statement.otherConnection ? "other connection: " : "")
                                      .arg(statement.fullScanSteps)
                                      .arg(statement.autoIndexes)
                                      .arg(statement.vmSteps)
                                      .arg(statement.sql.left(240)));
        }
        const auto others = std::count_if(statements.cbegin(), statements.cend(),
                                          [](const Statement& s) { return s.otherConnection; });
        qInfo().noquote() << operation << "statements" << statements.size() << "on-other" << others
                          << "vm-steps" << vmSteps;
        // The fork reads its source through its own snapshot connection.
        if (operation == "fork")
            QVERIFY(others > 0);
        QVERIFY2(!statements.isEmpty(), "The operation ran no statement on the journal");
        QVERIFY2(violations.isEmpty(), qPrintable("\n" + violations.join('\n')));
    }

  private:
    void exec(const QString& sql) {
        QSqlQuery q(db_);
        QVERIFY2(q.exec(sql), qPrintable(q.lastError().text() + ": " + sql.left(120)));
    }

    // `count` rows of prompts, deltas and command items, numbered per session from 1.
    void insertHistory(const QString& session, const QString& cursor, int count) {
        const QString key = "(" + cursor + ")";
        const QString type = "CASE " + key +
                             "%10 WHEN 1 THEN 'user.message' WHEN 2 THEN 'turn.started' WHEN 3 "
                             "THEN 'item.started' WHEN 4 THEN 'command.output_delta' WHEN 5 THEN "
                             "'item.completed' WHEN 0 THEN 'turn.completed' ELSE "
                             "'assistant.delta' END";
        const QString item = "'i'||(" + key + "/10)";
        const QString payload =
            "CASE " + key + "%10 WHEN 1 THEN json_object('text','prompt '||i) WHEN 3 THEN " +
            "json_object('item_id'," + item + ",'item',json_object('id'," + item +
            ",'type','commandExecution','command','make')) WHEN 4 THEN json_object('item_id'," +
            item + ",'delta',printf('%.200c','o')) WHEN 5 THEN json_object('item_id'," + item +
            ",'item',json_object('id'," + item +
            ",'type','commandExecution','status','completed')) "
            "ELSE json_object('text',printf('%.80c','a')) END";
        const QString columns = session + ",1," + key + "," + type + "," + payload +
                                ",strftime('%Y-%m-%dT%H:%M:%fZ','now'),1+(" + key + "-1)/10";
        exec(sqlRange(count) +
             "INSERT INTO events(session_id,generation,cursor,event_type,payload,sent_at,"
             "turn_sequence) SELECT " +
             columns + " FROM n");
        exec(sqlRange(count) +
             "INSERT INTO replay_events(session_id,generation,cursor,event_type,payload,sent_at,"
             "turn_sequence,item_id) SELECT " +
             columns + "," + item + " FROM n");
    }

    // Settles counters, projection and activity heads of sessions written by insertHistory.
    void settleHistory(const QString& where) {
        exec("UPDATE sessions SET last_cursor=(SELECT MAX(cursor) FROM events e WHERE "
             "e.session_id=sessions.id),current_turn_sequence=(SELECT MAX(turn_sequence) FROM "
             "events e WHERE e.session_id=sessions.id) WHERE " +
             where);
        exec("INSERT INTO replay_projection_state(session_id,generation,first_cursor,last_cursor) "
             "SELECT id,1,1,last_cursor FROM sessions WHERE " +
             where);
        exec("INSERT INTO activity_heads SELECT id,1,current_turn_sequence,0,last_cursor FROM "
             "sessions WHERE " +
             where);
    }

    // Many other conversations, each with its own short history.
    void writeBackground() {
        exec("BEGIN");
        exec(sqlRange(backgroundSessions) +
             "INSERT INTO sessions(id,worktree_path,adapter,thread_id,turn_id,state,generation,"
             "last_cursor) SELECT 'b'||i,'/tmp/w'||(i%20),'codex','','','ready',1,0 FROM n");
        insertHistory(QString("'b'||(1+i%%1)").arg(backgroundSessions),
                      QString("1+(i-1)/%1").arg(backgroundSessions), backgroundRows);
        settleHistory("id LIKE 'b%'");
        exec(sqlRange(tombstones) + "INSERT INTO session_tombstones SELECT 'gone'||i,1,1,'op'||i,"
                                    "strftime('%Y-%m-%dT%H:%M:%fZ','now') FROM n");
        exec("COMMIT");
    }

    // The conversation in use: a long history, then recent turns written through the journal,
    // including command output large enough to be stored as large events.
    void writeHotSession() {
        QVERIFY(journal_->upsertSession({"hot", dir_.path(), "codex", {}, {}, "ready"}));
        exec("BEGIN");
        insertHistory("'hot'", "i", hotBacklogRows);
        settleHistory("id='hot'");
        exec("COMMIT");
        exec("ANALYZE");
        for (int turn = 1; turn <= 30; ++turn) {
            QVERIFY(journal_->appendEvent("hot", 1, "user.message", {{"text", "go"}}) > 0);
            QVERIFY(journal_->appendEvent("hot", 1, "turn.started", {}) > 0);
            for (int n = 0; n < 4; ++n) {
                const QString id = QString("c%1-%2").arg(turn).arg(n);
                QJsonObject item{{"id", id}, {"type", "commandExecution"}};
                QVERIFY(journal_->appendEvent("hot", 1, "item.started",
                                              {{"item_id", id}, {"item", item}}) > 0);
                QVERIFY(journal_->appendEvent("hot", 1, "command.output_delta",
                                              {{"item_id", id}, {"delta", "line\n"}}) > 0);
                QString error;
                QVERIFY2(journal_->appendEvent("hot", 1, "item.completed",
                                               {{"item_id", id}, {"item", item}}, &error) > 0,
                         qPrintable(error));
            }
            QVERIFY(journal_->appendEvent("hot", 1, "assistant.delta", {{"text", "done"}}) > 0);
            QVERIFY(journal_->appendEvent("hot", 1, "turn.completed", {{"status", "completed"}}) >
                    0);
        }
        writeLargeResults();
        exec("UPDATE journal_gc_state SET payload_row=0,chunk_row=0");
    }

    // Tool results large enough to be stored as large events, in a conversation whose raw
    // events were already acknowledged and pruned. Three lose their transcript rows, as
    // pruned history leaves them, so the garbage pass must look each one up.
    void writeLargeResults() {
        QVERIFY(journal_->upsertSession({"media", dir_.path(), "codex", {}, {}, "ready"}));
        const QString large(300 * 1024, QChar('L'));
        for (int n = 0; n < 6; ++n) {
            QVERIFY(journal_->appendEvent("media", 1, "user.message", {{"text", "fetch"}}) > 0);
            const QString id = QString("r%1").arg(n);
            const QJsonObject result{
                {"id", id}, {"type", "mcpToolCall"}, {"tool", "fetch"}, {"result", large}};
            QString error;
            QVERIFY2(journal_->appendEvent("media", 1, "item.completed",
                                           {{"item_id", id}, {"item", result}}, &error) > 0,
                     qPrintable(error));
        }
        exec("DELETE FROM events WHERE session_id='media'");
        QSqlQuery q(db_);
        QVERIFY(q.exec("SELECT transfer_id FROM large_event_payloads ORDER BY rowid"));
        QStringList transfers;
        while (q.next())
            transfers.append(q.value(0).toString());
        QCOMPARE(transfers.size(), 6);
        transferId_ = transfers.constLast();
        for (const auto& transfer : transfers.mid(0, 3)) {
            QVERIFY(q.prepare("DELETE FROM replay_events WHERE session_id='media' AND "
                              "json_extract(payload,'$.large_event.transfer_id')=?"));
            q.addBindValue(transfer);
            QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
        }
    }

    QHash<QString, std::function<void()>> operations() {
        LocalJournal& j = *journal_;
        const auto head = [&j] { return j.sessionHead("hot").cursor; };
        const auto hotSession = [&j] {
            for (const auto& session : j.sessions())
                if (session.id == "hot")
                    return session;
            return StoredSession{};
        };
        return {
            {"append",
             [&j] {
                 const QJsonObject item{{"id", "late"}, {"type", "commandExecution"}};
                 QVERIFY(j.appendEvent("hot", 1, "item.started",
                                       {{"item_id", "late"}, {"item", item}}) > 0);
             }},
            {"turn-start",
             [&j, hotSession] {
                 const auto session = hotSession();
                 QVERIFY(!j.appendTurnStart(session, {{"text", "again"}}, {}).isEmpty());
             }},
            {"events-after", [&j] { QVERIFY(!j.eventsAfter("hot", 1, 10, 500).isEmpty()); }},
            {"compacted-events-after", [&j] { (void)j.compactedEventsAfter("hot", 1, 10, 500); }},
            {"transcript-page", [&j] { (void)j.transcriptPage("hot", 1, 0, 3); }},
            {"transcript-window",
             [&j] { QVERIFY(!j.transcriptWindow("hot", 1).events.isEmpty()); }},
            {"transcript-window-after",
             [&j] {
                 const auto window = j.transcriptWindow("hot", 1);
                 (void)j.transcriptWindowAfter("hot", 1, 10, window.snapshotCursor,
                                               window.projectionRevision);
             }},
            {"activity-summary",
             [&j] {
                 (void)j.activitySummaryEvent({{"session_id", "hot"},
                                               {"session_generation", 1},
                                               {"turn_sequence", 3},
                                               {"event_type", "item.started"},
                                               {"payload", QJsonObject{{"item_id", "c3-1"}}}});
             }},
            {"activity-group-page",
             [this, &j] {
                 QSqlQuery q(db_);
                 QVERIFY(q.exec("SELECT group_cursor FROM activity_items WHERE session_id='hot' "
                                "AND group_cursor>0 LIMIT 1") &&
                         q.next());
                 const auto group = q.value(0).toString();
                 q.finish();
                 QVERIFY(j.activityGroupPage("hot", 1, {{"group_id", group}}).value("ok").toBool());
             }},
            {"command-output", [&j] { (void)j.commandOutput("hot", 1, 3, "c3-1"); }},
            {"session-head", [head] { QVERIFY(head() > 0); }},
            {"open-turn", [&j] { (void)j.hasOpenTurn("hot", 1); }},
            {"mark-seen", [&j, head] { QVERIFY(j.markSessionSeen("hot", 1, head())); }},
            {"acknowledge",
             [&j, head] { QVERIFY(j.acknowledgeCursor("desktop", "hot", 1, head())); }},
            {"sessions", [&j] { QVERIFY(j.sessions().size() > backgroundSessions); }},
            {"deletion-work", [&j] { (void)j.deletionWork(); }},
            {"tombstone-page", [&j] { (void)j.sessionTombstones(); }},
            {"fork",
             [&j] {
                 StoredSession copy{"hot-copy", "/tmp", "codex", {}, {}, "ready"};
                 QString error;
                 auto staged = j.beginFork("hot", 1, copy, &error);
                 QVERIFY2(staged, qPrintable(error));
                 // The copy reads its source through its own snapshot connection.
                 sqlite3* reader = sqliteHandle(QSqlDatabase::database(staged->readerName, false));
                 sqlite3_trace_v2(reader, SQLITE_TRACE_PROFILE, recordStatement, nullptr);
                 auto step = LocalJournal::ForkStep::Progress;
                 while (step == LocalJournal::ForkStep::Progress)
                     step = j.continueFork(*staged, &error);
                 sqlite3_trace_v2(reader, 0, nullptr, nullptr);
                 QCOMPARE(step, LocalJournal::ForkStep::Copied);
                 QVERIFY2(j.finishFork(*staged, copy, &error), qPrintable(error));
             }},
            {"delete-and-purge",
             [&j] {
                 QString error;
                 QVERIFY2(j.deleteSession("b7", 1, &error), qPrintable(error));
                 for (int pass = 0; pass < 10 && j.hasPendingHistoryPurge(); ++pass)
                     (void)j.purgeDeletedSessionHistory();
             }},
            {"activity-backfill",
             [this, &j] {
                 exec("DELETE FROM activity_items WHERE session_id='b9'");
                 exec("DELETE FROM activity_heads WHERE session_id='b9'");
                 exec("INSERT INTO activity_backfill(session_id,generation) VALUES('b9',1)");
                 QCOMPARE(j.backfillActivityGroups(), LocalJournal::ActivityBackfill::Progress);
             }},
            {"activity-files-backfill",
             [this, &j] {
                 // Five groups of 20k items, so pages fall inside one group, and a listing
                 // marker near the end: the pass must seek to it, not walk the items before.
                 // Only the pass is audited.
                 auto* const audited = std::exchange(recording, nullptr);
                 QVERIFY(j.upsertSession({"files", dir_.path(), "codex", {}, {}, "ready"}));
                 exec(sqlRange(100000) +
                      "INSERT INTO activity_items(session_id,generation,turn_sequence,item_id,"
                      "first_cursor,last_cursor,group_cursor,family,running,failed) SELECT "
                      "'files',1,i,'f'||i,i,i,1+((i-1)/20000)*20000,'command',0,0 FROM n");
                 exec("DELETE FROM activity_backfill");
                 exec("INSERT INTO activity_files_backfill(session_id,generation,after_group,"
                      "after_cursor) VALUES('files',1,80001,99000)");
                 recording = audited;
                 QCOMPARE(j.backfillActivityGroups(), LocalJournal::ActivityBackfill::Progress);
                 QVERIFY(!j.hasPendingActivityBackfill());
             }},
            {"retention", [&j] { QVERIFY(j.pruneRetention()); }},
            {"payload-garbage",
             [this, &j] {
                 QVERIFY(j.pruneLargePayloadGarbage());
                 QSqlQuery q(db_);
                 QVERIFY(q.exec("SELECT COUNT(*) FROM large_event_payloads") && q.next());
                 // The pass looked the orphans up and removed them.
                 QCOMPARE(q.value(0).toInt(), 3);
             }},
            {"large-event-fragment",
             [this, &j] {
                 QVERIFY(j.largeEventFragment("media", 1, transferId_, 0).errorCode.isEmpty());
             }},
            {"acknowledged-events", [&j] { QVERIFY(j.pruneAcknowledgedEvents()); }},
            {"disk-usage", [&j] { QVERIFY(j.diskUsage().has_value()); }},
        };
    }

    QTemporaryDir dir_;
    QString path_;
    std::unique_ptr<LocalJournal> journal_;
    QSqlDatabase db_;
    QString transferId_;
    bool traced_{false};
};

QTEST_GUILESS_MAIN(JournalQueryAuditTest)
#include "JournalQueryAuditTest.moc"
