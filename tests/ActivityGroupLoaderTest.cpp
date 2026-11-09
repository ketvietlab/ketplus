#include "agent/ActivityGroupLoader.h"
#include <QJsonArray>
#include <QtTest>

using namespace ketplus;
namespace {
QJsonObject page(qint64 cursor, const QJsonArray& events = {}, bool more = false) {
    return {{"ok", true},
            {"session_id", "session"},
            {"group_id", "2"},
            {"events", events},
            {"next_cursor", cursor},
            {"snapshot_cursor", more ? 10 : cursor},
            {"projection_revision", 0},
            {"has_more", more}};
}
QJsonObject activityEvent(qint64 cursor, const QString& id, const QString& status = "inProgress") {
    return {{"cursor", cursor},
            {"turn_sequence", 1},
            {"event_type", status == "inProgress" ? "item.started" : "item.completed"},
            {"payload", QJsonObject{{"item", QJsonObject{{"id", id},
                                                         {"type", "commandExecution"},
                                                         {"command", "echo synthetic"},
                                                         {"status", status}}}}}};
}
} // namespace
class ActivityGroupLoaderTest : public QObject {
    Q_OBJECT
  private slots:
    void openCoalescesAndUpsertsWithoutDuplicates() {
        QList<ActivityGroupLoader::Reply> replies;
        QList<QJsonObject> requests;
        ActivityGroupLoader loader([&](const QString&, const QJsonObject& params, auto reply) {
            requests.append(params);
            replies.append(reply);
        });
        loader.setOpen("session", "2", 2, true);
        QCOMPARE(requests.size(), 1);
        loader.updateSummary("session", "2", 4);
        loader.updateSummary("session", "2", 8);
        QCOMPARE(requests.size(), 1);
        replies.takeFirst()(page(2, {activityEvent(2, "one")}), {});
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(requests.last().value("after_cursor").toInteger(), 2);
        replies.takeFirst()(
            page(8, {activityEvent(4, "one", "completed"), activityEvent(8, "two")}), {});
        QTest::qWait(10);
        QCOMPARE(requests.size(), 2);
        const auto rows = loader.details("session", "2").value("groupItems").toList();
        QCOMPARE(rows.size(), 2);
        QVERIFY(!rows.first().toMap().value("running").toBool());
    }
    void aStepLeavingTheGroupReloadsItsDetails() {
        QList<ActivityGroupLoader::Reply> replies;
        QList<QJsonObject> requests;
        ActivityGroupLoader loader([&](const QString&, const QJsonObject& params, auto reply) {
            requests.append(params);
            replies.append(reply);
        });
        loader.setOpen("session", "2", 3, true);
        replies.takeFirst()(page(3, {activityEvent(2, "one"), activityEvent(3, "shot")}), {});
        QTRY_COMPARE(loader.details("session", "2").value("groupItems").toList().size(), 2);
        // The same watermark with a newer membership still drops "shot" and reads again.
        loader.updateSummary("session", "2", 3, 5);
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(requests.last().value("after_cursor").toInteger(), 0);
        QVERIFY(loader.details("session", "2").value("groupItems").toList().isEmpty());
        replies.takeFirst()(page(5, {activityEvent(2, "one")}), {});
        QTRY_COMPARE(loader.details("session", "2").value("groupItems").toList().size(), 1);
        // A summary that repeats the membership changes nothing.
        loader.updateSummary("session", "2", 5, 5);
        QTest::qWait(10);
        QCOMPARE(requests.size(), 2);
    }
    void collapseFencesReplyAndReopenWaitsForTheInflightRequest() {
        QList<ActivityGroupLoader::Reply> replies;
        int calls = 0;
        ActivityGroupLoader loader([&](const QString&, const QJsonObject&, auto reply) {
            ++calls;
            replies.append(reply);
        });
        loader.setOpen("session", "2", 2, true);
        loader.setOpen("session", "2", 2, false);
        loader.setOpen("session", "2", 5, true);
        QCOMPARE(calls, 1);
        replies.takeFirst()(page(2, {activityEvent(2, "late")}), {});
        QTRY_COMPARE(calls, 2);
        QVERIFY(loader.details("session", "2").value("groupItems").toList().isEmpty());
        loader.setOpen("session", "2", 5, false);
        replies.takeFirst()(page(5, {activityEvent(5, "late")}), {});
        QTest::qWait(10);
        QCOMPARE(calls, 2);
        QVERIFY(loader.details("session", "2").value("groupItems").toList().isEmpty());
    }
    void pagesUseFenceAndExpiredProjectionRestartsSnapshot() {
        QList<ActivityGroupLoader::Reply> replies;
        QList<QJsonObject> requests;
        ActivityGroupLoader loader([&](const QString&, const QJsonObject& params, auto reply) {
            requests.append(params);
            replies.append(reply);
        });
        loader.setOpen("session", "2", 10, true);
        replies.takeFirst()(page(2, {activityEvent(2, "one")}, true), {});
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(requests.last().value("snapshot_cursor").toInteger(), 10);
        replies.takeFirst()({{"ok", false}, {"error_code", "snapshot_changed"}}, {});
        QTRY_COMPARE(requests.size(), 3);
        QVERIFY(!requests.last().contains("snapshot_cursor"));
        QCOMPARE(requests.last().value("after_cursor").toInteger(), 0);
        replies.takeFirst()(page(10, {activityEvent(2, "one", "completed")}), {});
        QTest::qWait(10);
        QCOMPARE(requests.size(), 3);
    }
    void failedReadIsVisibleAndExplicitRetryWorks() {
        QList<ActivityGroupLoader::Reply> replies;
        int calls = 0;
        ActivityGroupLoader loader([&](const QString&, const QJsonObject&, auto reply) {
            ++calls;
            replies.append(reply);
        });
        loader.setOpen("session", "2", 2, true);
        replies.takeFirst()({}, "Disconnected");
        QCOMPARE(loader.details("session", "2").value("error"), "Disconnected");
        loader.updateSummary("session", "2", 4);
        QTest::qWait(10);
        QCOMPARE(calls, 1);
        loader.setOpen("session", "2", 4, true);
        QCOMPARE(calls, 2);
        replies.takeFirst()(page(4), {});
        QVERIFY(loader.details("session", "2").value("error").toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ActivityGroupLoaderTest)
#include "ActivityGroupLoaderTest.moc"
