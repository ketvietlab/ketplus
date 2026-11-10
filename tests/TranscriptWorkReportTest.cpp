#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"

#include <QJsonArray>
#include <QQuickView>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QJsonObject journalEvent(const qint64 cursor, const qint64 turn, const QString& type,
                         const QJsonObject& payload) {
    return {
        {"cursor", cursor}, {"turn_sequence", turn}, {"event_type", type}, {"payload", payload}};
}

QJsonObject reportItem(const QString& key, const QString& group, const QString& status,
                       const qint64 created, const qint64 updated, QJsonObject fields = {}) {
    fields.insert("key", key);
    fields.insert("group", group);
    fields.insert("title", "Item " + key);
    fields.insert("status", status);
    fields.insert("created_turn", created);
    fields.insert("updated_turn", updated);
    return fields;
}

} // namespace

// ADR 0106: the turn's last work report closes the turn as the mock's card. The app counts the
// items itself, so cancelled work shows but is not counted, and an earlier report call in the
// same turn is replaced by the later one.
void AgentWorkspaceViewTest::turnEndShowsTheTurnsWorkReport() {
    const QJsonObject early{{"title", "Picker"},
                            {"turn", 2},
                            {"version", 1},
                            {"items", QJsonArray{reportItem("a", "PRs", "active", 2, 2)}}};
    const QJsonObject last{
        {"title", "Picker"},
        {"turn", 2},
        {"version", 2},
        {"items",
         QJsonArray{reportItem("a", "PRs", "done", 2, 2, {{"evidence", "Merged"}}),
                    reportItem("b", "PRs", "done", 1, 1, {{"evidence", "12/12"}}),
                    reportItem("c", "Decisions", "waiting", 1, 2,
                               {{"next", QJsonObject{{"action", "Choose"}, {"owner", "user"}}}}),
                    reportItem("d", "Dropped", "cancelled", 1, 1)}}};
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{journalEvent(1, 2, "user.message", {{"text", "Finish it"}}),
                               journalEvent(2, 2, "work_report.updated", early),
                               journalEvent(3, 2, "work_report.updated", last),
                               journalEvent(4, 2, "turn.completed", {{"status", "completed"}}),
                               journalEvent(5, 3, "user.message", {{"text", "Thanks"}}),
                               journalEvent(6, 3, "turn.completed", {{"status", "completed"}})}}});
    // The report rides on its turn's end row; no row of its own.
    int ends = 0;
    for (const QVariant& value : rows) {
        const QVariantMap row = value.toMap();
        QVERIFY(row.value("eventType") != QStringLiteral("work_report.updated"));
        if (row.value("subtype") != QStringLiteral("turnEnd"))
            continue;
        ++ends;
        const QVariantMap report = row.value("workReport").toMap();
        if (ends == 1)
            QCOMPARE(report.value("version").toInt(), 2);
        else
            QVERIFY(report.isEmpty());
    }
    QCOMPARE(ends, 2);
    // Copy conversation carries the report as the matrix it replaced, counted the same way.
    const QString copied = ketplus::agent_message::transcriptText(rows);
    QVERIFY2(copied.contains("Picker · 2/3 done"), qPrintable(copied));
    QVERIFY2(copied.contains("| Item a | Done | Merged |  |"), qPrintable(copied));
    QVERIFY2(copied.contains("| Item c | Waiting |  | Choose · You |"), qPrintable(copied));

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 900);
    view.show();
    view.resetMessages(rows, "work-report");
    auto* root = view.quickView()->rootObject();
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(root, "transcriptWorkReport")) != nullptr);
    QCOMPARE(card->property("total").toInt(), 3);
    QCOMPARE(card->property("finished").toInt(), 2);
    QVERIFY(card->property("needsYou").toBool());
    const QVariantList groups = card->property("groups").toList();
    QCOMPARE(groups.size(), 3);
    QCOMPARE(groups.at(0).toMap().value("title").toString(), QStringLiteral("PRs"));
    // New and Updated come from the turn numbers, not from the agent.
    QVariant change;
    QVERIFY(QMetaObject::invokeMethod(
        card, "change", Q_RETURN_ARG(QVariant, change),
        Q_ARG(QVariant, last.value("items").toArray().at(0).toVariant())));
    QCOMPARE(change.toString(), QStringLiteral("new"));
    QVERIFY(QMetaObject::invokeMethod(
        card, "change", Q_RETURN_ARG(QVariant, change),
        Q_ARG(QVariant, last.value("items").toArray().at(2).toVariant())));
    QCOMPARE(change.toString(), QStringLiteral("updated"));
    QVERIFY(QMetaObject::invokeMethod(
        card, "change", Q_RETURN_ARG(QVariant, change),
        Q_ARG(QVariant, last.value("items").toArray().at(1).toVariant())));
    QCOMPARE(change.toString(), QString());
    // A cancelled-only group reads Cancelled instead of a work state.
    QVariant state;
    QVERIFY(QMetaObject::invokeMethod(card, "groupStatus", Q_RETURN_ARG(QVariant, state),
                                      Q_ARG(QVariant, groups.at(2).toMap().value("items"))));
    QCOMPARE(state.toString(), QStringLiteral("cancelled"));
    // Only the turn that reported shows a card.
    int cards = 0;
    const auto countCards = [&](const auto& self, QQuickItem* item) -> void {
        if (item->objectName() == QStringLiteral("transcriptWorkReport") && item->isVisible())
            ++cards;
        for (auto* child : item->childItems())
            self(self, child);
    };
    countCards(countCards, root);
    QCOMPARE(cards, 1);
    const auto capture = qEnvironmentVariable("KETPLUS_WORK_REPORT_CAPTURE");
    if (!capture.isEmpty()) {
        card->setProperty("openItem", "c");
        QTest::qWait(120);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}
