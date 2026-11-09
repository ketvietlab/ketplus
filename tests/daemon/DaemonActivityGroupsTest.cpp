#include "storage/LocalJournal.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
namespace {
QJsonObject command(const QString& id, const QString& status = "inProgress") {
    return {{"item", QJsonObject{{"id", id},
                                 {"type", "commandExecution"},
                                 {"command", "private command"},
                                 {"status", status}}}};
}
QJsonObject browser(const QString& id, const QString& tool) {
    return {{"item", QJsonObject{{"id", id},
                                 {"type", "mcpToolCall"},
                                 {"server", "ketplus_browser"},
                                 {"tool", tool}}}};
}
QJsonObject fileChange(const QString& id, const QJsonArray& changes,
                       const QString& status = "completed") {
    return {{"item",
             QJsonObject{
                 {"id", id}, {"type", "fileChange"}, {"status", status}, {"changes", changes}}}};
}
QJsonObject change(const QString& path, const QString& kind, const QString& diff) {
    return {{"path", path}, {"kind", QJsonObject{{"type", kind}}}, {"diff", diff}};
}
bool seed(LocalJournal& journal, const QString& directory) {
    return journal.upsertSession({"session", directory, "codex", {}, {}, "ready"}) &&
           journal.appendEvent("session", 1, "user.message", {{"text", "Synthetic"}}) > 0;
}
QJsonObject last(LocalJournal& journal, qint64 cursor) {
    return journal.eventsAfter("session", 1, cursor - 1, 1).first().toObject();
}
} // namespace
class DaemonActivityGroupsTest : public QObject {
    Q_OBJECT
  private slots:
    void settlementSeeksOnlyRunningItemsInCurrentTurn() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, dir.path()));
        QVERIFY(journal.appendEvent("session", 1, "item.started", command("running")) > 0);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(
            q.exec("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                   "INSERT INTO activity_items SELECT 'session',1,x%2,'old-'||x,1000+x,1000+x,"
                   "1000+x,'command',CASE WHEN x=2 THEN 1 ELSE 0 END,0 FROM n"));
        QVERIFY(
            q.exec("WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                   "INSERT INTO activity_summaries SELECT 'session',1,1000+x,'command',1,"
                   "CASE WHEN x=2 THEN 1 ELSE 0 END,0,1000+x,x%2 FROM n"));
        for (const auto& table : {QString("activity_items"), QString("activity_summaries")}) {
            QSqlQuery plan(db);
            QVERIFY(plan.prepare(
                QString("EXPLAIN QUERY PLAN UPDATE %1 SET running=0,last_cursor=? "
                        "WHERE session_id=? AND generation=? AND turn_sequence=? AND running>0")
                    .arg(table)));
            for (const QVariant& value :
                 QVariantList{qint64(3), QString("session"), qint64(1), qint64(1)})
                plan.addBindValue(value);
            QVERIFY(plan.exec());
            QString details;
            while (plan.next())
                details += plan.value(3).toString();
            QVERIFY2(details.contains(table + "_running_turn"), qPrintable(details));
            QVERIFY2(details.contains("turn_sequence=?"), qPrintable(details));
        }
        QElapsedTimer elapsed;
        elapsed.start();
        for (int n = 0; n < 25; ++n)
            QVERIFY(journal.appendEvent("session", 1, "turn.completed", {{"status", "completed"}}) >
                    0);
        qInfo("25 production settlements with 100000 historical rows: %lld ms", elapsed.elapsed());
        for (const auto& table : {QString("activity_items"), QString("activity_summaries")}) {
            QVERIFY(q.exec(QString("SELECT count(*) FROM %1 WHERE session_id='session' AND "
                                   "generation=1 AND turn_sequence=1 AND running>0")
                               .arg(table)));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);
            QVERIFY(q.exec(QString("SELECT count(*) FROM %1 WHERE session_id='session' AND "
                                   "generation=1 AND turn_sequence=0 AND running>0")
                               .arg(table)));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            // Upgrade path must recreate the additive index on an older journal.
            QVERIFY(q.exec(QString("DROP INDEX %1_running_turn").arg(table)));
        }
        q.finish();
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QVERIFY(q.exec("SELECT count(*) FROM sqlite_master WHERE type='index' AND name IN "
                       "('activity_items_running_turn','activity_summaries_running_turn')"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
    }

    void summaryOmitsDetailsAndOutputAdvancesCursor() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(journal.isOpen());
        QVERIFY(seed(journal, dir.path()));
        const auto first = journal.appendEvent("session", 1, "item.started", command("one"));
        QVERIFY(first > 0);
        const auto summary = journal.activitySummaryEvent(last(journal, first));
        QCOMPARE(summary.value("event_type"), "activity.group.summary");
        QVERIFY(!QJsonDocument(summary).toJson().contains("private command"));
        const auto group = summary.value("payload").toObject().value("group_id").toString();
        const auto output = journal.appendEvent("session", 1, "command.output_delta",
                                                {{"item_id", "one"}, {"delta", "private output"}});
        const auto updated =
            journal.activitySummaryEvent(last(journal, output)).value("payload").toObject();
        QCOMPARE(updated.value("count").toInt(), 1);
        QCOMPARE(updated.value("last_change_cursor").toInteger(), output);
        QVERIFY(!QJsonDocument(updated).toJson().contains("private output"));
        auto page = journal.activityGroupPage("session", 1, {{"group_id", group}});
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("events").toArray().size(), 2);
        QCOMPARE(
            journal.eventsAfter("session", 1, first - 1, 1).first().toObject().value("payload"),
            QJsonValue(command("one")));
        auto incremental =
            journal.activityGroupPage("session", 1, {{"group_id", group}, {"after_cursor", first}});
        QCOMPARE(incremental.value("events").toArray().size(), 1);
        QCOMPARE(incremental.value("events").toArray().first().toObject().value("event_type"),
                 "command.output_delta");
    }
    void searchesReadsEditsAndCommandsShareOneTurnGroup() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        const auto first =
            journal.appendEvent("session", 1, "item.completed", command("build", "completed"));
        QVERIFY(first > 0);
        const auto group = journal.activitySummaryEvent(last(journal, first))
                               .value("payload")
                               .toObject()
                               .value("group_id");
        const auto tool = [](const QString& id, const QString& type, const QString& name) {
            return QJsonObject{
                {"item",
                 QJsonObject{{"id", id}, {"type", type}, {"tool", name}, {"status", "completed"}}}};
        };
        const QList<QJsonObject> steps{
            tool("read", "mcpToolCall", "functions__read_file"),
            tool("search", "dynamicToolCall", "search_code"),
            {{"item",
              QJsonObject{{"id", "edit"}, {"type", "fileChange"}, {"status", "completed"}}}},
            command("test", "completed"),
        };
        for (const auto& step : steps) {
            const auto cursor = journal.appendEvent("session", 1, "item.completed", step);
            QVERIFY(cursor > 0);
            const auto summary = journal.activitySummaryEvent(last(journal, cursor));
            QCOMPARE(summary.value("event_type"), "activity.group.summary");
            QCOMPARE(summary.value("payload").toObject().value("group_id"), group);
        }
        const auto page = journal.activityGroupPage("session", 1, {{"group_id", group}});
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("events").toArray().size(), 5);
        QCOMPARE(journal.activitySummaryEvent(last(journal, first))
                     .value("payload")
                     .toObject()
                     .value("count")
                     .toInt(),
                 5);
    }
    void toolServersWaitsAndSubagentsJoinTheGroupInFirstUseOrder() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        const auto item = [](const QString& id, const QString& type, const QString& tool,
                             const QString& server = {}) {
            QJsonObject body{{"id", id}, {"type", type}, {"tool", tool}, {"status", "completed"}};
            if (!server.isEmpty())
                body.insert("server", server);
            return QJsonObject{{"item", body}};
        };
        const QList<QJsonObject> steps{
            command("build", "completed"),
            item("agents-1", "mcpToolCall", "agents_list", "ketplus_agents"),
            browser("observe", "browser_observe"),
            item("agents-2", "mcpToolCall", "agents_send", "ketplus_agents"),
            item("spawn", "collabAgentToolCall", "spawnAgent"),
            item("wait-agent", "collabAgentToolCall", "wait"),
            item("sleep", "dynamicToolCall", "wait"),
        };
        qint64 first = 0;
        for (const auto& step : steps) {
            const auto cursor = journal.appendEvent("session", 1, "item.completed", step);
            QVERIFY(cursor > 0);
            if (!first)
                first = cursor;
        }
        const auto summary =
            journal.activitySummaryEvent(last(journal, first)).value("payload").toObject();
        QCOMPARE(summary.value("count").toInt(), 7);
        QStringList families;
        for (const auto& value : summary.value("families").toArray())
            families.append(QStringLiteral("%1=%2")
                                .arg(value.toObject().value("family").toString())
                                .arg(value.toObject().value("count").toInt()));
        QCOMPARE(families, (QStringList{"command=1", "tool:ketplus_agents=2", "browser=1",
                                        "subagent=1", "subagent.step=1", "wait=1"}));
        // Other tools keep their own rows: their results may hold images, or carry edits the
        // summary would drop (Claude's Write).
        for (const auto& standalone : {item("capture", "mcpToolCall", "capture", "screen_tools"),
                                       item("write", "dynamicToolCall", "Write"),
                                       item("repl", "mcpToolCall", "js", "node_repl")}) {
            const auto cursor = journal.appendEvent("session", 1, "item.completed", standalone);
            QCOMPARE(
                journal.activitySummaryEvent(last(journal, cursor)).value("event_type").toString(),
                "item.completed");
        }
        // A skill stays visible on its own, as in a local transcript.
        const auto skill = journal.appendEvent("session", 1, "item.completed",
                                               item("skill", "dynamicToolCall", "Skill"));
        QCOMPARE(journal.activitySummaryEvent(last(journal, skill)).value("event_type").toString(),
                 "item.completed");
    }
    void evidenceAndMessagesBreakGroupsAndLateCompletionKeepsIdentity() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        const auto one = journal.appendEvent("session", 1, "item.started",
                                             browser("observe", "browser_observe"));
        const auto group = journal.activitySummaryEvent(last(journal, one))
                               .value("payload")
                               .toObject()
                               .value("group_id");
        for (const auto& tool :
             {QStringLiteral("browser_capture"), QStringLiteral("visual_compare")}) {
            const auto capture =
                journal.appendEvent("session", 1, "item.started", browser(tool, tool));
            QCOMPARE(journal.activitySummaryEvent(last(journal, capture)).value("event_type"),
                     "item.started");
        }
        const auto next =
            journal.appendEvent("session", 1, "item.started", browser("next", "browser_observe"));
        QVERIFY(journal.activitySummaryEvent(last(journal, next))
                    .value("payload")
                    .toObject()
                    .value("group_id") != group);
        const auto completed = journal.appendEvent("session", 1, "item.completed",
                                                   browser("observe", "browser_observe"));
        QCOMPARE(journal.activitySummaryEvent(last(journal, completed))
                     .value("payload")
                     .toObject()
                     .value("group_id"),
                 group);
        QVERIFY(journal.appendEvent("session", 1, "approval.requested", {{"approval_id", "q"}}) >
                0);
        const auto third = journal.appendEvent("session", 1, "item.started", command("three"));
        QVERIFY(journal.activitySummaryEvent(last(journal, third))
                    .value("payload")
                    .toObject()
                    .value("group_id") != group);
    }
    void screenshotObservationAndTerminalStateStayConsistent() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        auto screenshot = browser("screenshot", "browser_observe");
        auto item = screenshot.value("item").toObject();
        item.insert("arguments", QJsonObject{{"includeScreenshot", true}});
        screenshot.insert("item", item);
        const auto image = journal.appendEvent("session", 1, "item.started", screenshot);
        QCOMPARE(journal.activitySummaryEvent(last(journal, image)).value("event_type"),
                 "item.started");
        const auto cursor = journal.appendEvent("session", 1, "item.started", command("one"));
        const auto end =
            journal.appendEvent("session", 1, "turn.completed", {{"status", "completed"}});
        const auto summary =
            journal.activitySummaryEvent(last(journal, cursor)).value("payload").toObject();
        QCOMPARE(summary.value("running_count").toInt(), 0);
        QCOMPARE(summary.value("last_change_cursor").toInteger(), end);
        const auto page =
            journal.activityGroupPage("session", 1, {{"group_id", QString::number(cursor)}});
        QVERIFY(page.value("ok").toBool());
        QVERIFY(!page.value("group_running").toBool());
        QCOMPARE(page.value("events").toArray().size(), 1);
    }
    // ADR 0022: a group's summary lists the files its edits changed, merged by path in the
    // order first changed and bounded, without any diff text.
    void editSummaryListsChangedFiles() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        QVERIFY(journal.appendEvent(
                    "session", 1, "item.completed",
                    fileChange("one", {change("src/a.cpp", "update", "@@ -1 +1 @@\n-a\n+b"),
                                       change("src/b.cpp", "add", "x\ny")})) > 0);
        QVERIFY(journal.appendEvent("session", 1, "item.completed",
                                    fileChange("two",
                                               {change("src/c.cpp", "update", "@@ -1 +1 @@\n+c")},
                                               "failed")) > 0);
        const auto cursor = journal.appendEvent(
            "session", 1, "item.completed",
            fileChange("three", {change("src/a.cpp", "update", "@@ -4 +4 @@\n+again")}));
        QVERIFY(cursor > 0);
        const auto summary =
            journal.activitySummaryEvent(last(journal, cursor)).value("payload").toObject();
        QCOMPARE(summary.value("files_total").toInt(), 2);
        const auto files = summary.value("files").toArray();
        QCOMPARE(files.size(), 2);
        QCOMPARE(files.at(0).toObject(), (QJsonObject{{"path", "src/a.cpp"},
                                                      {"destination", ""},
                                                      {"operation", "Edited"},
                                                      {"added", 2},
                                                      {"removed", 1},
                                                      {"known", true}}));
        QCOMPARE(files.at(1).toObject().value("operation").toString(), "Created");
        QCOMPARE(files.at(1).toObject().value("added").toInt(), 2);
        QVERIFY(!QJsonDocument(summary).toJson().contains("+again"));

        QJsonArray many;
        for (int i = 0; i < 60; ++i)
            many.append(change(QString("gen/%1.cpp").arg(i), "add", "x"));
        const auto bulk =
            journal.appendEvent("session", 1, "item.completed", fileChange("bulk", many));
        const auto bounded =
            journal.activitySummaryEvent(last(journal, bulk)).value("payload").toObject();
        QCOMPARE(bounded.value("files").toArray().size(), 50);
        QCOMPARE(bounded.value("files_total").toInt(), 62);
    }
    // A path's later changes decide its operation; a file created and deleted again drops out.
    void editSummaryMergesEachPathsOperations() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        auto moved = change("src/mv.cpp", "update", "@@ -1 +1 @@\n+m");
        moved.insert("kind", QJsonObject{{"type", "update"}, {"move_path", "src/moved.cpp"}});
        QVERIFY(journal.appendEvent(
                    "session", 1, "item.completed",
                    fileChange("one", {change("src/new.cpp", "add", "a"),
                                       change("src/old.cpp", "update", "@@ -1 +1 @@\n+o"),
                                       change("src/tmp.cpp", "add", "t"),
                                       change("src/mv.cpp", "update", "@@ -1 +1 @@\n+e")})) > 0);
        const auto cursor = journal.appendEvent(
            "session", 1, "item.completed",
            fileChange("two", {change("src/new.cpp", "update", "@@ -1 +1 @@\n+b"),
                               change("src/old.cpp", "delete", "o"),
                               change("src/tmp.cpp", "delete", "t"), moved}));
        QVERIFY(cursor > 0);
        const auto summary =
            journal.activitySummaryEvent(last(journal, cursor)).value("payload").toObject();
        QCOMPARE(summary.value("files_total").toInt(), 3);
        QHash<QString, QJsonObject> byPath;
        for (const auto& value : summary.value("files").toArray())
            byPath.insert(value.toObject().value("path").toString(), value.toObject());
        QCOMPARE(byPath.size(), 3);
        QCOMPARE(byPath.value("src/new.cpp").value("operation").toString(), "Created");
        QCOMPARE(byPath.value("src/old.cpp").value("operation").toString(), "Deleted");
        QCOMPARE(byPath.value("src/mv.cpp").value("operation").toString(), "Moved");
        QCOMPARE(byPath.value("src/mv.cpp").value("destination").toString(), "src/moved.cpp");
        QVERIFY(!byPath.contains("src/tmp.cpp"));
    }
    // The files keep their order when a completion arrives again or the session is forked.
    void editSummaryOrderSurvivesRedeliveryAndFork() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        const auto one = fileChange("one", {change("src/a.cpp", "update", "@@ -1 +1 @@\n+a")});
        QVERIFY(journal.appendEvent("session", 1, "item.started", one) > 0);
        QVERIFY(journal.appendEvent("session", 1, "item.completed", one) > 0);
        const auto two = journal.appendEvent(
            "session", 1, "item.completed",
            fileChange("two", {change("src/b.cpp", "update", "@@ -1 +1 @@\n+b")}));
        QVERIFY(two > 0);
        const auto paths = [&](const QString& session, qint64 cursor) {
            QStringList listed;
            const auto event = journal.eventsAfter(session, 1, cursor - 1, 1).first().toObject();
            for (const auto& value : journal.activitySummaryEvent(event)
                                         .value("payload")
                                         .toObject()
                                         .value("files")
                                         .toArray())
                listed.append(value.toObject().value("path").toString());
            return listed;
        };
        const QStringList expected{"src/a.cpp", "src/b.cpp"};
        QCOMPARE(paths("session", two), expected);
        const auto again = journal.appendEvent("session", 1, "item.completed", one);
        QVERIFY(again > 0);
        QCOMPARE(paths("session", again), expected);
        StoredSession fork{"fork", dir.path(), "codex", {}, {}, "ready"};
        QVERIFY(journal.forkSession("session", 1, fork));
        QCOMPARE(paths("fork", again), expected);
    }
    // A backfill that did not finish runs again on the next open.
    void interruptedFilesBackfillRunsAgain() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        qint64 cursor;
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, dir.path()));
            cursor = journal.appendEvent(
                "session", 1, "item.completed",
                fileChange("one", {change("src/a.cpp", "update", "@@ -1 +1 @@\n+b")}));
            QVERIFY(cursor > 0);
            QVERIFY(journal.appendEvent("session", 1, "item.completed",
                                        command("two", "completed")) > 0);
        }
        const auto connection = QStringLiteral("activity-files-interrupt-fixture");
        const auto exec = [&](const QStringList& statements) {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            bool ok = db.open();
            for (const auto& sql : statements) {
                QSqlQuery query(db);
                ok = ok && query.exec(sql);
            }
            db.close();
            return ok;
        };
        // An old journal with one event left to index, whose indexing fails this time.
        QVERIFY(exec({"DROP TABLE activity_files",
                      QString("UPDATE activity_heads SET indexed_cursor=%1").arg(cursor),
                      "CREATE TRIGGER interrupt BEFORE UPDATE ON activity_heads BEGIN SELECT "
                      "RAISE(ABORT,'interrupted'); END"}));
        QSqlDatabase::removeDatabase(connection);
        {
            LocalJournal journal(path);
            QVERIFY(!journal.isOpen());
        }
        QVERIFY(exec({"DROP TRIGGER interrupt"}));
        QSqlDatabase::removeDatabase(connection);
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        const auto files = journal.activitySummaryEvent(last(journal, cursor))
                               .value("payload")
                               .toObject()
                               .value("files")
                               .toArray();
        QCOMPARE(files.size(), 1);
        QCOMPARE(files.first().toObject().value("path").toString(), "src/a.cpp");
    }
    // A journal indexed before activity_files existed lists its groups' files once.
    void existingJournalBackfillsChangedFiles() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        qint64 cursor;
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, dir.path()));
            cursor = journal.appendEvent(
                "session", 1, "item.completed",
                fileChange("one", {change("src/a.cpp", "update", "@@ -1 +1 @@\n+b")}));
            QVERIFY(cursor > 0);
        }
        const auto connection = QStringLiteral("activity-files-backfill-fixture");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("DROP TABLE activity_files"));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        for (int pass = 0; pass < 2; ++pass) {
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            const auto files = journal.activitySummaryEvent(last(journal, cursor))
                                   .value("payload")
                                   .toObject()
                                   .value("files")
                                   .toArray();
            QCOMPARE(files.size(), 1);
            QCOMPARE(files.first().toObject().value("path").toString(), "src/a.cpp");
            QCOMPARE(files.first().toObject().value("added").toInt(), 1);
        }
    }
    void existingJournalBackfillsOnceWithoutChangingCanonicalEvents() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        qint64 first;
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, dir.path()));
            first = journal.appendEvent("session", 1, "item.started", command("one"));
            QVERIFY(journal.appendEvent("session", 1, "item.completed",
                                        command("one", "completed")) > 0);
        }
        const auto connection = QStringLiteral("activity-backfill-fixture");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            for (const auto table : {"activity_items", "activity_summaries", "activity_heads"})
                QVERIFY(query.exec(QString("DROP TABLE %1").arg(table)));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        for (int pass = 0; pass < 2; ++pass) {
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            const auto summary =
                journal.activitySummaryEvent(last(journal, first)).value("payload").toObject();
            QCOMPARE(summary.value("count").toInt(), 1);
            QCOMPARE(summary.value("running_count").toInt(), 0);
            QCOMPARE(journal.activityGroupPage("session", 1, {{"group_id", QString::number(first)}})
                         .value("events")
                         .toArray()
                         .size(),
                     2);
            QCOMPARE(last(journal, first).value("payload"), QJsonValue(command("one")));
        }
    }
    void snapshotPagesFenceNewEventsAndCompaction() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        QVERIFY(seed(journal, dir.path()));
        const auto first = journal.appendEvent("session", 1, "item.started", command("one"));
        QVERIFY(journal.appendEvent("session", 1, "command.output_delta",
                                    {{"item_id", "one"}, {"delta", "old"}}) > 0);
        const auto group = QString::number(first);
        const auto page =
            journal.activityGroupPage("session", 1, {{"group_id", group}, {"event_limit", 1}});
        QVERIFY(page.value("has_more").toBool());
        const auto newer = journal.appendEvent("session", 1, "item.started", command("two"));
        QJsonObject continuation{{"group_id", group},
                                 {"event_limit", 1},
                                 {"after_cursor", page.value("next_cursor")},
                                 {"snapshot_cursor", page.value("snapshot_cursor")},
                                 {"projection_revision", page.value("projection_revision")}};
        const auto second = journal.activityGroupPage("session", 1, continuation);
        QVERIFY(!second.value("has_more").toBool());
        QVERIFY(second.value("next_cursor").toInteger() < newer);
        auto item = command("one", "completed");
        auto body = item.value("item").toObject();
        body.insert("aggregatedOutput", "old");
        item.insert("item", body);
        QVERIFY(journal.appendEvent("session", 1, "item.completed", item) > 0);
        QCOMPARE(journal.activityGroupPage("session", 1, continuation).value("error_code"),
                 "snapshot_changed");
        QVERIFY(
            journal.activityGroupPage("session", 1, {{"group_id", group}}).value("ok").toBool());
        QCOMPARE(journal.activityGroupPage("session", 2, {{"group_id", group}}).value("error_code"),
                 "stale_generation");
        QCOMPARE(
            journal.activityGroupPage("session", 1, {{"group_id", group}, {"after_cursor", 99999}})
                .value("error_code"),
            "invalid_page");
    }
    void restartForkAndDeleteRetainIsolation() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        qint64 cursor;
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, dir.path()));
            cursor = journal.appendEvent("session", 1, "item.started", command("one"));
        }
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        const auto group = QString::number(cursor);
        QVERIFY(
            journal.activityGroupPage("session", 1, {{"group_id", group}}).value("ok").toBool());
        StoredSession fork{"fork", dir.path(), "codex", {}, {}, "ready"};
        QVERIFY(journal.forkSession("session", 1, fork));
        QVERIFY(journal.activityGroupPage("fork", 1, {{"group_id", group}}).value("ok").toBool());
        QVERIFY(journal.deleteSession("session", 1));
        QVERIFY(
            !journal.activityGroupPage("session", 1, {{"group_id", group}}).value("ok").toBool());
        QVERIFY(journal.activityGroupPage("fork", 1, {{"group_id", group}}).value("ok").toBool());
    }
};
QTEST_GUILESS_MAIN(DaemonActivityGroupsTest)
#include "DaemonActivityGroupsTest.moc"
