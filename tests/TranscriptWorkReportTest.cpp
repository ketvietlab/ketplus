#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/WorkReportPane.h"

#include <QJsonArray>
#include <QQuickView>
#include <QSignalSpy>
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
    // The card's footer and the header's Work report button open the Report tab.
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::workReportRequested);
    const auto click = [&](QQuickItem* item) {
        QVERIFY(item != nullptr && item->isVisible());
        const QPointF center = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(view.quickView(), Qt::LeftButton, {}, center.toPoint());
    };
    click(findVisibleItem(card, "workReportFooter"));
    QTRY_COMPARE(opened.count(), 1);
    click(findVisibleItem(root, "headerWorkReportButton"));
    QTRY_COMPARE(opened.count(), 2);
    const auto capture = qEnvironmentVariable("KETPLUS_WORK_REPORT_CAPTURE");
    if (!capture.isEmpty()) {
        card->setProperty("openItem", "c");
        QTest::qWait(120);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}

// ADR 0106: turn.completed repeats the report, so a page that begins after the turn's
// work_report.updated events still shows the card and copies the table.
void AgentWorkspaceViewTest::turnEndReportSurvivesAHistoryPageSplit() {
    const QJsonObject report{
        {"title", "Picker"},
        {"turn", 4},
        {"version", 3},
        {"items", QJsonArray{reportItem("a", "PRs", "done", 4, 4, {{"evidence", "Merged"}})}}};
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{journalEvent(40, 4, "agent.message", {{"text", "Done"}}),
                               journalEvent(41, 4, "turn.completed",
                                            {{"status", "completed"}, {"work_report", report}})}}});
    QVariantMap end;
    for (const QVariant& value : rows)
        if (value.toMap().value("subtype") == QStringLiteral("turnEnd"))
            end = value.toMap();
    QCOMPARE(end.value("workReport").toMap().value("version").toInt(), 3);
    const QString copied = ketplus::agent_message::transcriptText(rows);
    QVERIFY2(copied.contains("| Item a | Done | Merged |  |"), qPrintable(copied));
}

// Group names are the agent's free text, including names an object's prototype already has.
void AgentWorkspaceViewTest::workReportGroupsAcceptAnyName() {
    QJsonArray items;
    const QStringList names{"constructor", "toString", "__proto__", "hasOwnProperty"};
    for (const QString& name : names)
        items.append(reportItem(name.toLower(), name, "active", 1, 1));
    items.append(reportItem("again", "constructor", "done", 1, 1, {{"evidence", "ok"}}));
    const QJsonObject report{{"title", "Odd"}, {"turn", 1}, {"version", 1}, {"items", items}};
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{journalEvent(1, 1, "user.message", {{"text", "Go"}}),
                               journalEvent(2, 1, "turn.completed",
                                            {{"status", "completed"}, {"work_report", report}})}}});
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 900);
    view.show();
    view.resetMessages(rows, "work-report-groups");
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(view.quickView()->rootObject(), "transcriptWorkReport")) !=
                nullptr);
    const QVariantList groups = card->property("groups").toList();
    QCOMPARE(groups.size(), names.size());
    for (qsizetype index = 0; index < names.size(); ++index)
        QCOMPARE(groups.at(index).toMap().value("title").toString(), names.at(index));
    QCOMPARE(groups.at(0).toMap().value("items").toList().size(), 2);
    QCOMPARE(card->property("total").toInt(), 5);
    // Collapsing a group named like a prototype member keeps its own state.
    QVariant open;
    QVERIFY(QMetaObject::invokeMethod(card, "groupOpen", Q_RETURN_ARG(QVariant, open),
                                      Q_ARG(QVariant, groups.at(0))));
    QVERIFY(open.toBool());
    QVERIFY(QMetaObject::invokeMethod(card, "toggleGroup", Q_ARG(QVariant, groups.at(0))));
    QVERIFY(QMetaObject::invokeMethod(card, "groupOpen", Q_RETURN_ARG(QVariant, open),
                                      Q_ARG(QVariant, groups.at(0))));
    QVERIFY(!open.toBool());
    QVERIFY(QMetaObject::invokeMethod(card, "groupOpen", Q_RETURN_ARG(QVariant, open),
                                      Q_ARG(QVariant, groups.at(1))));
    QVERIFY(open.toBool());
}

// ADR 0106: the Report tab shows one conversation's ledger; a late read or another
// conversation's report never replaces what it shows.
void AgentWorkspaceViewTest::workReportPaneFollowsItsConversation() {
    const auto ledger = [](const qint64 version, const QString& status) {
        return QJsonObject{
            {"title", "Picker"},
            {"turn", 3},
            {"version", version},
            {"items", QJsonArray{reportItem("a", "PRs", status, 1, 3, {{"evidence", "Merged"}})}}}
            .toVariantMap();
    };
    ketplus::WorkReportPane pane;
    pane.resize(600, 700);
    pane.show();
    auto* root = pane.quickView()->rootObject();
    QVERIFY(root != nullptr);
    pane.showConversation("first");
    QVERIFY(pane.loading());
    QQuickItem* empty = nullptr;
    QTRY_VERIFY((empty = findVisibleItem(root, "workReportEmpty")) != nullptr);
    QVERIFY(empty->property("text").toString().startsWith("Loading"));

    pane.acceptReport("other", ledger(9, "active"), {});
    QVERIFY(pane.report().isEmpty());
    pane.acceptReport("first", ledger(2, "done"), {});
    QVERIFY(!pane.loading());
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(root, "workReportLedger")) != nullptr);
    QVERIFY(card->property("ledger").toBool());
    QCOMPARE(card->property("finished").toInt(), 1);
    // The ledger view has no footer link back to itself.
    QVERIFY(!findVisibleItem(card, "workReportFooter")->property("opens").toBool());
    if (const auto capture = qEnvironmentVariable("KETPLUS_WORK_REPORT_PANE_CAPTURE");
        !capture.isEmpty()) {
        QTest::qWait(120);
        QVERIFY(pane.quickView()->grabWindow().save(capture));
    }
    pane.acceptReport("first", ledger(1, "active"), {});
    QCOMPARE(pane.report().value("version").toInt(), 2);
    pane.acceptReport("first", {}, "The daemon is restarting.");
    QCOMPARE(pane.report().value("version").toInt(), 2);
    QVERIFY(pane.message().isEmpty());

    pane.showConversation("second");
    QVERIFY(pane.report().isEmpty());
    pane.acceptReport("second", {}, {});
    QTRY_VERIFY((empty = findVisibleItem(root, "workReportEmpty")) != nullptr);
    QVERIFY(empty->property("text").toString().startsWith("No work report yet"));
    pane.acceptReport("second", {}, "Update the daemon to see the work report.");
    QTRY_COMPARE(empty->property("text").toString(),
                 QStringLiteral("Update the daemon to see the work report."));
}
