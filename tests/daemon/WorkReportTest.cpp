#include "agents/WorkReport.h"
#include "agents/AgentBroker.h"
#include "agents/AgentInstructions.h"
#include "agents/AgentMcpServer.h"
#include "cloud/CloudEventProjection.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/CursorAdapter.h"
#include "runtime/ResponseGuidance.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "AgentTestFixture.h"

#include <QJsonArray>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using namespace agent_test;

namespace {

QJsonObject item(const QString& key, const QString& status, QJsonObject fields = {}) {
    fields.insert("key", key);
    fields.insert("status", status);
    if (!fields.contains("title"))
        fields.insert("title", "Item " + key);
    if (!fields.contains("group"))
        fields.insert("group", "Work");
    return fields;
}

QJsonObject call(const QJsonArray& items, const QString& title = {}) {
    QJsonObject value{{"items", items}};
    if (!title.isEmpty())
        value.insert("title", title);
    return value;
}

QJsonObject found(const QJsonObject& ledger, const QString& key) {
    for (const QJsonValue& value : ledger.value("items").toArray())
        if (value.toObject().value("key").toString() == key)
            return value.toObject();
    return {};
}

} // namespace

class WorkReportTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void mergesByKeyAndKeepsUnmentionedItems() {
        const auto first = work_report::merge(
            {},
            call({item("pr-399", "done",
                       {{"evidence", "Merged"}, {"verified", QJsonArray{"tests"}}}),
                  item("picker", "active",
                       {{"next", QJsonObject{{"action", "Run its tests"}, {"owner", "agent"}}}})},
                 "Repository picker"),
            3);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        QCOMPARE(first.added, 2);
        QCOMPARE(first.ledger.value("version").toInteger(), 1);
        QCOMPARE(first.ledger.value("title").toString(), QStringLiteral("Repository picker"));
        QCOMPARE(found(first.ledger, "picker").value("created_turn").toInteger(), 3);

        // Turn 4 changes one item and adds another; the third is not mentioned.
        const auto second =
            work_report::merge(first.ledger,
                               call({item("picker", "done", {{"evidence", "Dialog tests 12/12"}}),
                                     item("adr", "waiting", {{"title", "ADR 0106"}})}),
                               4);
        QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
        QCOMPARE(second.added, 1);
        QCOMPARE(second.updated, 1);
        QCOMPARE(second.ledger.value("version").toInteger(), 2);
        QCOMPARE(second.ledger.value("title").toString(), QStringLiteral("Repository picker"));
        const QJsonArray items = second.ledger.value("items").toArray();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(0).toObject().value("key").toString(), QStringLiteral("pr-399"));
        QCOMPARE(found(second.ledger, "pr-399").value("updated_turn").toInteger(), 3);
        const QJsonObject picker = found(second.ledger, "picker");
        QCOMPARE(picker.value("status").toString(), QStringLiteral("done"));
        QCOMPARE(picker.value("created_turn").toInteger(), 3);
        QCOMPARE(picker.value("updated_turn").toInteger(), 4);
        // A field the call leaves out keeps its value.
        QCOMPARE(picker.value("next").toObject().value("action").toString(),
                 QStringLiteral("Run its tests"));

        // Repeating an item unchanged is not an update.
        const auto same = work_report::merge(
            second.ledger, call({item("adr", "waiting", {{"title", "ADR 0106"}})}), 5);
        QCOMPARE(same.updated, 0);
        QCOMPARE(found(same.ledger, "adr").value("updated_turn").toInteger(), 4);
    }

    void cancelledItemsStayVisibleButAreNotCounted() {
        const auto merged = work_report::merge({},
                                               call({item("a", "done", {{"evidence", "Merged"}}),
                                                     item("b", "active"), item("c", "cancelled")}),
                                               1);
        QVERIFY2(merged.error.isEmpty(), qPrintable(merged.error));
        QCOMPARE(merged.ledger.value("items").toArray().size(), 3);
        const QJsonObject summary = work_report::summary(merged);
        QCOMPARE(summary.value("done").toInt(), 1);
        QCOMPARE(summary.value("total").toInt(), 2);
        QCOMPARE(summary.value("open").toInt(), 1);
    }

    void rejectsInvalidCallsWithAFix_data() {
        QTest::addColumn<QJsonObject>("payload");
        QTest::addColumn<QString>("message");
        QTest::newRow("empty") << call({}) << "at least one item";
        QTest::newRow("done without evidence")
            << call({item("a", "done")}) << "done without evidence";
        QTest::newRow("unknown status") << call({item("a", "finished")}) << "unknown status";
        QTest::newRow("duplicate key")
            << call({item("a", "active"), item("a", "waiting")}) << "appears twice";
        QTest::newRow("invalid key") << call({item("Not a key", "active")}) << "is invalid";
        QTest::newRow("owner")
            << call({item("a", "active",
                          {{"next", QJsonObject{{"action", "Ship"}, {"owner", "someone"}}}})})
            << "next.owner";
        QTest::newRow("check") << call({item("a", "done", {{"verified", QJsonArray{"vibes"}}})})
                               << "unknown check";
        QTest::newRow("new item without a title")
            << call({QJsonObject{{"key", "a"}, {"status", "active"}, {"group", "Work"}}})
            << "needs a title";
        QTest::newRow("new item without a status")
            << call({QJsonObject{{"key", "a"}, {"title", "A"}, {"group", "Work"}}})
            << "needs a status";
        QJsonArray many;
        for (int index = 0; index <= work_report::maximumItemsPerCall; ++index)
            many.append(item(QStringLiteral("i%1").arg(index), "active"));
        QTest::newRow("too many in one call") << call(many) << "at most 50";
    }
    void rejectsInvalidCallsWithAFix() {
        QFETCH(QJsonObject, payload);
        QFETCH(QString, message);
        const auto merged = work_report::merge({}, payload, 1);
        QVERIFY(merged.ledger.isEmpty());
        QVERIFY2(merged.error.contains(message, Qt::CaseInsensitive), qPrintable(merged.error));
    }

    void redactsAndCapsTextBeforeStorageAndRelay() {
        const auto merged = work_report::merge(
            {},
            call({item(
                "a", "blocked",
                {{"evidence", "curl -H 'Authorization: Bearer abc.def' token=s3cret"},
                 {"remaining", "Run with --api-key sk-live-123\nthen retry"},
                 {"title", QString(500, QLatin1Char('x'))},
                 {"next", QJsonObject{{"action", "Use password=hunter2"}, {"owner", "#8213"}}}})}),
            1);
        QVERIFY2(merged.error.isEmpty(), qPrintable(merged.error));
        const QJsonObject stored = found(merged.ledger, "a");
        const QString all = QString::fromUtf8(QJsonDocument(stored).toJson());
        for (const QString& secret : {"abc.def", "s3cret", "sk-live-123", "hunter2"})
            QVERIFY2(!all.contains(secret), qPrintable(all));
        QVERIFY(stored.value("remaining").toString().contains("then retry"));
        QVERIFY(!stored.value("remaining").toString().contains('\n'));
        QCOMPARE(stored.value("title").toString().size(), work_report::maximumTitle);
        QCOMPARE(stored.value("next").toObject().value("owner").toString(), QStringLiteral("8213"));

        // A ledger written by an older daemon is redacted again on its way to the cloud.
        QJsonObject legacy = merged.ledger;
        QJsonArray items = legacy.value("items").toArray();
        QJsonObject raw = items.first().toObject();
        raw.insert("evidence", "token=leaked");
        items[0] = raw;
        legacy.insert("items", items);
        const QString projected = QString::fromUtf8(
            QJsonDocument(projectEventPayloadForCloud("work_report.updated", legacy)).toJson());
        QVERIFY2(!projected.contains("leaked"), qPrintable(projected));
    }

    void closedItemsLeaveFirstWhenTheLedgerIsFull() {
        QJsonObject ledger;
        qint64 turn = 1;
        for (int batch = 0; batch < 4; ++batch) {
            QJsonArray items;
            for (int index = 0; index < 50; ++index)
                items.append(item(QStringLiteral("b%1-%2").arg(batch).arg(index),
                                  batch == 0 ? "done" : "active", {{"evidence", "ok"}}));
            const auto merged = work_report::merge(ledger, call(items), turn++);
            QVERIFY2(merged.error.isEmpty(), qPrintable(merged.error));
            ledger = merged.ledger;
        }
        QCOMPARE(ledger.value("items").toArray().size(), work_report::maximumItems);
        // One more open item pushes out the oldest done item.
        const auto more = work_report::merge(ledger, call({item("extra", "active")}), turn++);
        QVERIFY2(more.error.isEmpty(), qPrintable(more.error));
        QCOMPARE(more.ledger.value("items").toArray().size(), work_report::maximumItems);
        QVERIFY(found(more.ledger, "b0-0").isEmpty());
        QVERIFY(!found(more.ledger, "extra").isEmpty());
        // When everything is open, nothing leaves silently.
        QJsonObject open = more.ledger;
        QJsonArray reopen;
        for (const QJsonValue& value : open.value("items").toArray())
            if (value.toObject().value("status") == "done")
                reopen.append(
                    QJsonObject{{"key", value.toObject().value("key")}, {"status", "active"}});
        open = work_report::merge(open, call(reopen), turn++).ledger;
        const auto full = work_report::merge(open, call({item("one-more", "active")}), turn);
        QVERIFY(full.ledger.isEmpty());
        QVERIFY2(full.error.contains("open items"), qPrintable(full.error));
    }

    void guidanceUsesTheToolWhenItIsAvailable() {
        const QString matrix = responseGuidance(false);
        const QString report = responseGuidance(true);
        QVERIFY(matrix.contains("End with a concise progress matrix"));
        QVERIFY(!matrix.contains("work_report"));
        QVERIFY(report.contains("record them with the work_report tool"));
        QVERIFY(report.contains("do not repeat it as a table or matrix"));
        // The matrix stays the fallback when the tool fails.
        QVERIFY(report.contains("If work_report is unavailable or fails"));
        QVERIFY(!report.contains("End with a concise progress matrix"));
        QVERIFY(report.contains("remaining tasks in the work report"));
        QVERIFY(report.startsWith("<ketplus_response_preferences>\n"));
        QVERIFY(report.endsWith("\n</ketplus_response_preferences>"));
        QCOMPARE(withResponseGuidance({}), matrix);
        QCOMPARE(withResponseGuidance("Host", true), QStringLiteral("Host\n\n") + report);
        // Agent-started turns keep their single reply.
        QVERIFY(agent_messaging::automaticTurnGuidance({}).contains(
            "Do not add a progress matrix or call work_report"));
    }

    // Each provider asks for the report only when it was given the ketplus_agents server.
    void providersAskForTheReportOnlyWithTheirAgentServer_data() {
        QTest::addColumn<bool>("endpoint");
        QTest::newRow("agent server") << true;
        QTest::newRow("no agent server") << false;
    }
    void providersAskForTheReportOnlyWithTheirAgentServer() {
        QFETCH(bool, endpoint);
        Fixture fixture;
        QVERIFY(fixture.open(endpoint));
        const QString claude = fixture.start("claude", "hello");
        const QString codex = fixture.start("codex", "hello");
        const QString cursor = fixture.start("cursor", "hello");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(claude, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(codex, "turn.completed"), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(cursor, "turn.completed"), 1, 8000);
        for (const QString& path : {fixture.claudeLog, fixture.codexLog, fixture.cursorLog}) {
            const QByteArray log = logText(path);
            QVERIFY2(log.contains("ketplus_response_preferences"), qPrintable(path));
            QCOMPARE(log.contains("record them with the work_report tool"), endpoint);
            QCOMPARE(log.contains("End with a concise progress matrix"), !endpoint);
        }
    }

    void toolMergesIntoTheJournalAndTheTranscript() {
        Fixture fixture;
        QVERIFY(fixture.open());
        const QString id = fixture.start("claude", "Finish the picker");
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.count(id, "turn.completed"), 1, 5000);
        const QJsonObject first = fixture.agents(
            id, "agents.work_report",
            call({item("picker", "active"), item("pr-1", "done", {{"evidence", "Merged"}})},
                 "Picker"));
        QVERIFY2(first.value("ok").toBool(), qPrintable(text(first)));
        QCOMPARE(first.value("done").toInt(), 1);
        QCOMPARE(first.value("total").toInt(), 2);
        const QJsonObject second = fixture.agents(
            id, "agents.work_report", call({item("picker", "done", {{"evidence", "12/12"}})}));
        QVERIFY2(second.value("ok").toBool(), qPrintable(text(second)));
        QCOMPARE(second.value("version").toInteger(), 2);
        QCOMPARE(second.value("done").toInt(), 2);

        const QJsonObject stored = fixture.journal->workReport(id, 1);
        QCOMPARE(stored.value("version").toInteger(), 2);
        QCOMPARE(found(stored, "picker").value("status").toString(), QStringLiteral("done"));
        // Each call journals the ledger it produced, so a turn keeps its own snapshot.
        const QList<QJsonObject> snapshots = fixture.payloads(id, "work_report.updated");
        QCOMPARE(snapshots.size(), 2);
        QCOMPARE(found(snapshots.first(), "picker").value("status").toString(),
                 QStringLiteral("active"));
        QCOMPARE(snapshots.last(), stored);

        const QJsonObject rejected =
            fixture.agents(id, "agents.work_report", call({item("x", "done")}));
        QVERIFY(!rejected.value("ok").toBool());
        QCOMPARE(rejected.value("error_code").toString(), QStringLiteral("invalid_report"));
        QCOMPARE(fixture.count(id, "work_report.updated"), 2);
        QCOMPARE(fixture.journal->workReport(id, 1), stored);

        // The desktop reads the current ledger; only the local UI may.
        const QJsonObject read = fixture.host->handleCommand(
            request("session.work_report.read", {}, id, 1), "desktop-test");
        QVERIFY2(read.value("ok").toBool(), qPrintable(text(read)));
        QCOMPARE(read.value("report").toObject(), stored);
        QCOMPARE(
            fixture.host
                ->handleCommand(request("session.work_report.read", {}, id, 1), "cloud-control-v2")
                .value("error_code")
                .toString(),
            QStringLiteral("forbidden"));
        QVERIFY(protocol::features(1).contains("session.work_report/1"));

        // The ledger survives a restart, and a forged credential reaches nothing.
        fixture.restart();
        QCOMPARE(fixture.journal->workReport(id, 1), stored);
        QCOMPARE(
            fixture.host
                ->handleAgentCommand("forged", "agents.work_report", call({item("y", "active")}))
                .value("error_code")
                .toString(),
            QStringLiteral("authentication_failed"));
    }

    void ledgerForksAndIsDeletedWithItsConversation() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.isOpen());
        StoredSession source;
        source.id = "source";
        source.worktreePath = directory.path();
        source.adapter = "codex";
        source.state = "ready";
        QVERIFY(journal.upsertSession(source));
        QVERIFY(journal.appendEvent("source", 1, "user.message", {{"text", "hello"}}) > 0);
        const QJsonObject ledger =
            work_report::merge({}, call({item("a", "active")}, "Source"), 1).ledger;
        QVERIFY(journal.storeWorkReport("source", 1, ledger));
        // An older version never replaces a newer one.
        QJsonObject older = ledger;
        older.insert("version", 0);
        QString error;
        QVERIFY(!journal.storeWorkReport("source", 1, older, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(journal.workReport("source", 1), ledger);

        StoredSession fork = source;
        fork.id = "fork";
        fork.threadId = "fork-thread";
        QVERIFY2(journal.forkSession("source", 1, fork, &error), qPrintable(error));
        QCOMPARE(journal.workReport(fork.id, fork.generation), ledger);

        QVERIFY2(journal.deleteSession("source", 1, &error), qPrintable(error));
        QVERIFY(journal.workReport("source", 1).isEmpty());
        QCOMPARE(journal.workReport(fork.id, fork.generation), ledger);
    }

    void mcpServerListsAndRoutesTheTool() {
        QStringList operations;
        const agent_mcp::DaemonCall daemon = [&](const QString& operation, const QJsonObject&) {
            operations.append(operation);
            return QJsonObject{{"ok", true}, {"version", 1}, {"done", 0}, {"total", 1}};
        };
        const QJsonArray tools =
            agent_mcp::handleMessage({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}},
                                     daemon)
                .value("result")
                .toObject()
                .value("tools")
                .toArray();
        QJsonObject tool;
        for (const QJsonValue& value : tools)
            if (value.toObject().value("name") == "work_report")
                tool = value.toObject();
        QVERIFY(!tool.isEmpty());
        QVERIFY(tool.value("description").toString().contains("Never include secrets"));
        const QJsonObject schema = tool.value("inputSchema").toObject();
        QCOMPARE(schema.value("required").toArray(), (QJsonArray{"items"}));
        const QJsonObject itemSchema = schema.value("properties")
                                           .toObject()
                                           .value("items")
                                           .toObject()
                                           .value("items")
                                           .toObject();
        QCOMPARE(itemSchema.value("properties")
                     .toObject()
                     .value("status")
                     .toObject()
                     .value("enum")
                     .toArray()
                     .size(),
                 5);
        const QJsonObject answer = agent_mcp::handleMessage(
            {{"jsonrpc", "2.0"},
             {"id", 2},
             {"method", "tools/call"},
             {"params",
              QJsonObject{{"name", "work_report"}, {"arguments", call({item("a", "active")})}}}},
            daemon);
        QVERIFY(!answer.value("result").toObject().value("isError").toBool());
        QCOMPARE(operations, (QStringList{"agents.work_report"}));
        QVERIFY(AgentBroker::isOperation("agents.work_report"));
    }
};

QTEST_GUILESS_MAIN(WorkReportTest)
#include "WorkReportTest.moc"
