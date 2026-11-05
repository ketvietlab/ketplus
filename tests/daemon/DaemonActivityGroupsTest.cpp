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
