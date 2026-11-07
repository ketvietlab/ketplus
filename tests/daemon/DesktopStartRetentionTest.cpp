#include "agent/DaemonTranscriptProjection.h"
#include "storage/LocalJournal.h"
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;
class DesktopStartRetentionTest final : public QObject {
    Q_OBJECT
  private slots:
    void keepsProjectionOrderAndActivityIdentity_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("order");
        for (const char* kind :
             {"commandExecution", "mcpToolCall", "dynamicToolCall", "fileChange"})
            for (const char* order : {"adjacent", "interleaved", "reverse"})
                QTest::newRow(qPrintable(QString(kind) + "-" + order))
                    << QString(kind) << QString(order);
    }
    void keepsProjectionOrderAndActivityIdentity() {
        QFETCH(QString, kind);
        QFETCH(QString, order);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = "s";
        session.worktreePath = "synthetic";
        session.adapter = "codex";
        session.state = "ready";
        session.generation = 1;
        session.title = "Synthetic";
        QVERIFY(journal.upsertSession(session));
        QVERIFY(journal.appendEvent("s", 1, "user.message", {{"text", "old prompt"}}) > 0);
        QVERIFY(journal.appendEvent("s", 1, "turn.started", {}) > 0);
        const auto item = [&](const QString& id, bool completed) {
            QJsonObject fields{
                {"id", id}, {"type", kind}, {"status", completed ? "completed" : "inProgress"}};
            if (kind == "commandExecution") {
                fields.insert("command", "synthetic");
                fields.insert("aggregatedOutput", completed ? "short output" : "");
                fields.insert("exitCode", completed ? QJsonValue(0) : QJsonValue());
            } else if (kind == "fileChange") {
                fields.insert("changes",
                              QJsonArray{QJsonObject{{"path", id + ".cpp"},
                                                     {"kind", QJsonObject{{"type", "add"}}},
                                                     {"diff", "one line\n"}}});
            } else {
                fields.insert("server", "synthetic");
                fields.insert("tool", "read_file");
                fields.insert("arguments", QJsonObject{{"path", id + ".cpp"}});
                fields.insert("result", completed ? QJsonValue("short result") : QJsonValue());
            }
            return QJsonObject{{"provider", "codex"}, {"item", fields}};
        };
        const auto first = journal.appendEvent("s", 1, "item.started", item("a", false));
        QVERIFY(first > 0);
        if (order == "adjacent") {
            QVERIFY(journal.appendEvent("s", 1, "item.completed", item("a", true)) > 0);
            QVERIFY(journal.appendEvent("s", 1, "item.started", item("b", false)) > 0);
            QVERIFY(journal.appendEvent("s", 1, "item.completed", item("b", true)) > 0);
        } else {
            QVERIFY(journal.appendEvent("s", 1, "item.started", item("b", false)) > 0);
            const QString firstDone = order == "reverse" ? "b" : "a";
            const QString lastDone = order == "reverse" ? "a" : "b";
            QVERIFY(journal.appendEvent("s", 1, "item.completed", item(firstDone, true)) > 0);
            QVERIFY(journal.appendEvent("s", 1, "item.completed", item(lastDone, true)) > 0);
        }
        qint64 cursor = journal.appendEvent("s", 1, "turn.completed", {});
        for (int turn = 0; turn < 20; ++turn) {
            QVERIFY(journal.appendEvent("s", 1, "user.message", {{"text", "new prompt"}}) > 0);
            QVERIFY(journal.appendEvent("s", 1, "turn.started", {}) > 0);
            cursor = journal.appendEvent("s", 1, "turn.completed", {});
            QVERIFY(cursor > 0);
        }
        QVERIFY(journal.acknowledgeCursor("client", "s", 1, cursor));
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("UPDATE replay_events SET sent_at='2020-01-01T00:00:00.000Z'"));
        }
        QSqlDatabase::removeDatabase(connection);
        const auto before = journal.compactedEventsAfter("s", 1, 0, 500);
        const auto projected =
            ketplus::agent_message::daemonWindowMessages({{"events", before.events}});
        const auto groupBefore =
            journal.activityGroupPage("s", 1, {{"group_id", QString::number(first)}});
        const auto snapshot = journal.transcriptWindow("s", 1);
        const auto changed =
            journal.pruneReplayDetails(QDateTime::currentDateTimeUtc().addDays(-30));
        QCOMPARE(changed, std::optional<qint64>(order == "adjacent"  ? 2
                                                : order == "reverse" ? 1
                                                                     : 0));
        const auto after = journal.compactedEventsAfter("s", 1, 0, 500);
        QCOMPARE(ketplus::agent_message::daemonWindowMessages({{"events", after.events}}),
                 projected);
        QCOMPARE(after.nextCursor, before.nextCursor);
        auto page = journal.transcriptPage("s", 1, 0, 20);
        auto paged = journal.transcriptPage("s", 1, page.earliestTurnSequence, 20).events;
        for (const auto& event : page.events)
            paged.append(event);
        QCOMPARE(paged, after.events);
        const auto groupAfter =
            journal.activityGroupPage("s", 1, {{"group_id", QString::number(first)}});
        QVERIFY(groupBefore.value("ok").toBool());
        QVERIFY(groupAfter.value("ok").toBool());
        QCOMPARE(ketplus::agent_message::daemonWindowMessages(groupAfter),
                 ketplus::agent_message::daemonWindowMessages(groupBefore));
        if (order != "interleaved")
            QCOMPARE(journal
                         .transcriptWindow("s", 1, snapshot.beforeCursor, snapshot.snapshotCursor,
                                           snapshot.projectionRevision)
                         .errorCode,
                     "snapshot_changed");
        QCOMPARE(journal.pruneReplayDetails(QDateTime::currentDateTimeUtc().addDays(-30)),
                 std::optional<qint64>(0));
    }
};
QTEST_GUILESS_MAIN(DesktopStartRetentionTest)
#include "DesktopStartRetentionTest.moc"
