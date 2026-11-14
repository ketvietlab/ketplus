#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/ConversationChangesPane.h"
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

// The mock's report (mockups/qt Fixtures.turnReport) in the daemon's ledger shape, so the app's
// captures compare with the mock's side by side.
QJsonObject mockReport() {
    const auto item = [](const QString& key, const QString& group, const QString& title,
                         const QString& status, const qint64 created, const qint64 updated,
                         QJsonObject fields = {}) {
        QJsonObject result = reportItem(key, group, status, created, updated, fields);
        result.insert("title", title);
        return result;
    };
    const auto next = [](const QString& action, const QString& owner) {
        return QJsonObject{{"action", action}, {"owner", owner}};
    };
    return {
        {"title", "Repository picker and new UI follow-ups"},
        {"turn", 4},
        {"version", 7},
        {"items",
         QJsonArray{
             item("pr-399", "Pull requests", "#399 Interface zoom separate from font size", "done",
                  2, 3,
                  {{"evidence", "Merged into feat/new-ui · review by @agent/7542 passed"},
                   {"verified", QJsonArray{"tests", "build"}}}),
             item("pr-401", "Pull requests", "#401 Mermaid labels meet AA contrast", "done", 2, 3,
                  {{"evidence",
                    "The 4.45:1 label now reaches 4.6:1 · sweep regression added · merged"},
                   {"verified", QJsonArray{"code", "tests", "build", "capture"}}}),
             item("picker-daemon", "Repository picker · #164",
                  "The daemon lists another repository's worktrees", "done", 4, 4,
                  {{"evidence", "Lists them without switching or enrolling the repository · "
                                "daemon_worktree_context 2/2 passed"},
                   {"verified", QJsonArray{"code", "tests"}}}),
             item("picker-dialog", "Repository picker · #164", "The dialog follows the mock flow",
                  "active", 2, 4,
                  {{"evidence", "Choose a repository, then one of its worktrees, or use the "
                                "repository or create a worktree"},
                   {"remaining", "Worktree counts, the window's call site and the dialog tests"},
                   {"next", next("Finish the dialog and run its tests", "agent")},
                   {"verified", QJsonArray{"code"}}}),
             item("picker-adr", "Repository picker · #164", "ADR 0105 and the daemon version",
                  "waiting", 2, 2, {{"next", next("Write them once the dialog lands", "agent")}}),
             item("issue-402", "Decisions", "#402 Retire the compatibility stylesheet", "waiting",
                  4, 4,
                  {{"remaining", "Now, or folded into #137–#139"},
                   {"next", next("Choose when to do it", "user")}}),
             item("update-notice", "Decisions", "Toasts and the update notice", "blocked", 2, 3,
                  {{"remaining", "The notice needs approved wording"},
                   {"next", next("Approve the notice wording", "user")}}),
             item("legacy-picker", "Dropped", "Keep the old picker behind a setting", "cancelled",
                  1, 3, {{"evidence", "Replaced by the mock flow in turn 3"}})}}};
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
// An open item no call changed for work_report::staleTurns was likely forgotten by the agent:
// it reads muted with how long it waited, and does not make the report need you.
void AgentWorkspaceViewTest::forgottenItemsReadStaleAndDoNotNeedYou() {
    const QJsonObject report{
        {"title", "Team"},
        {"turn", 40},
        {"version", 9},
        {"items", QJsonArray{reportItem("first-assignments", "Plan", "waiting", 2, 2,
                                        {{"next", QJsonObject{{"action", "Press Start"},
                                                              {"owner", "user"}}}}),
                             reportItem("t-16", "Tasks", "active", 30, 39)}}};
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{journalEvent(1, 40, "user.message", {{"text", "Go on"}}),
                               journalEvent(2, 40, "work_report.updated", report),
                               journalEvent(3, 40, "turn.completed", {{"status", "completed"}})}}});
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 900);
    view.show();
    view.resetMessages(rows, "work-report-stale");
    auto* root = view.quickView()->rootObject();
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(root, "transcriptWorkReport")) != nullptr);
    QVERIFY(!card->property("needsYou").toBool());
    // A group of waiting items starts collapsed; open each to read its rows.
    for (const QVariant& group : card->property("groups").toList()) {
        QVariant open;
        QVERIFY(QMetaObject::invokeMethod(card, "groupOpen", Q_RETURN_ARG(QVariant, open),
                                          Q_ARG(QVariant, group)));
        if (!open.toBool())
            QVERIFY(QMetaObject::invokeMethod(card, "toggleGroup", Q_ARG(QVariant, group)));
    }
    QStringList badges;
    QTRY_VERIFY(([&] {
        badges.clear();
        const auto collect = [&](const auto& self, QQuickItem* item) -> void {
            if (item->objectName() == "workReportItemBadge" && item->isVisible())
                badges << item->property("text").toString();
            for (auto* child : item->childItems())
                self(self, child);
        };
        collect(collect, card);
        return !badges.isEmpty();
    })());
    QVERIFY2(badges.contains("No update for 38 turns"), qPrintable(badges.join(", ")));
    QVERIFY2(!badges.contains("No update for 1 turns"), qPrintable(badges.join(", ")));
}

// A long conversation's card: a duplicate another item replaced is neither shown nor counted,
// and items finished long ago fold under "Finished earlier" beside the group's recent work. A
// group with nothing recent shows its history unfolded.
void AgentWorkspaceViewTest::duplicatesHideAndOldFinishedItemsFold() {
    const QJsonObject merged{{"evidence", "Merged"}};
    QJsonObject oldDone = merged, recentDone = merged, replaced = merged;
    oldDone.insert("closed_turn", 5);
    recentDone.insert("closed_turn", 35);
    replaced.insert("replaced_by", "t-20");
    const QJsonObject report{
        {"title", "Team"},
        {"turn", 40},
        {"version", 12},
        {"items", QJsonArray{reportItem("old-done", "Tasks", "done", 2, 30, oldDone),
                             // Closed before closed_turn existed: dates from when it was added.
                             reportItem("legacy-done", "Tasks", "done", 3, 39, merged),
                             reportItem("recent-done", "Tasks", "done", 20, 35, recentDone),
                             reportItem("t-20", "Tasks", "active", 30, 38),
                             reportItem("t20-runbooks", "Tasks", "done", 4, 39, replaced),
                             reportItem("history", "History", "done", 1, 2, oldDone)}}};
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{journalEvent(1, 40, "user.message", {{"text", "Go on"}}),
                               journalEvent(2, 40, "work_report.updated", report),
                               journalEvent(3, 40, "turn.completed", {{"status", "completed"}})}}});
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 1200);
    view.show();
    view.resetMessages(rows, "work-report-fold");
    auto* root = view.quickView()->rootObject();
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(root, "transcriptWorkReport")) != nullptr);
    QCOMPARE(card->property("total").toInt(), 5);
    QCOMPARE(card->property("finished").toInt(), 4);
    QVariantList groups = card->property("groups").toList();
    QCOMPARE(groups.size(), 2);
    for (const QVariant& group : groups) {
        QVariant open;
        QVERIFY(QMetaObject::invokeMethod(card, "groupOpen", Q_RETURN_ARG(QVariant, open),
                                          Q_ARG(QVariant, group)));
        if (!open.toBool())
            QVERIFY(QMetaObject::invokeMethod(card, "toggleGroup", Q_ARG(QVariant, group)));
    }
    const auto visibleTexts = [&] {
        QStringList texts;
        const auto collect = [&](const auto& self, QQuickItem* item) -> void {
            if (!item->isVisible())
                return;
            if (const QVariant text = item->property("text"); text.isValid())
                texts << text.toString();
            for (auto* child : item->childItems())
                self(self, child);
        };
        collect(collect, card);
        return texts;
    };
    QTRY_VERIFY(visibleTexts().contains("Item history"));
    QStringList texts = visibleTexts();
    QVERIFY2(texts.contains("Item t-20") && texts.contains("Item recent-done"),
             qPrintable(texts.join(", ")));
    QVERIFY2(!texts.contains("Item t20-runbooks"), qPrintable(texts.join(", ")));
    QVERIFY2(!texts.contains("Item old-done") && !texts.contains("Item legacy-done"),
             qPrintable(texts.join(", ")));
    QVERIFY2(texts.count("Finished earlier (2)") == 1, qPrintable(texts.join(", ")));
    QVERIFY(QMetaObject::invokeMethod(card, "toggleEarlier", Q_ARG(QVariant, groups.at(0))));
    QTRY_VERIFY(visibleTexts().contains("Item old-done"));
    texts = visibleTexts();
    QVERIFY2(texts.contains("Item legacy-done") && texts.contains("Hide earlier finished items"),
             qPrintable(texts.join(", ")));
    QVERIFY2(!texts.contains("Item t20-runbooks"), qPrintable(texts.join(", ")));
}

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

    // What the user opened or closed stays with that conversation.
    const QVariantList firstGroups = card->property("groups").toList();
    QVERIFY(QMetaObject::invokeMethod(card, "toggleGroup", Q_ARG(QVariant, firstGroups.at(0))));
    card->setProperty("openItem", "a");
    pane.showConversation("other");
    pane.acceptReport("other", ledger(1, "active"), {});
    QTRY_VERIFY((card = findVisibleItem(root, "workReportLedger")) != nullptr);
    QCOMPARE(card->property("openItem").toString(), QString());
    QVariant open;
    QVERIFY(QMetaObject::invokeMethod(card, "groupOpen", Q_RETURN_ARG(QVariant, open),
                                      Q_ARG(QVariant, card->property("groups").toList().at(0))));
    QVERIFY(open.toBool());

    pane.showConversation("second");
    QVERIFY(pane.report().isEmpty());
    pane.acceptReport("second", {}, {});
    QTRY_VERIFY((empty = findVisibleItem(root, "workReportEmpty")) != nullptr);
    QVERIFY(empty->property("text").toString().startsWith("No work report yet"));
    pane.acceptReport("second", {}, "Update the daemon to see the work report.");
    QTRY_COMPARE(empty->property("text").toString(),
                 QStringLiteral("Update the daemon to see the work report."));
}

// The header's Changes opens this tab: the worktree's uncommitted files as the kit card, each
// opening its diff, and Open Source Control, as the mock's DetailsPane changes tab.
void AgentWorkspaceViewTest::changesPaneListsTheWorktreeChanges() {
    ketplus::ConversationChangesPane pane;
    pane.resize(600, 500);
    pane.show();
    auto* root = pane.quickView()->rootObject();
    QVERIFY(root != nullptr);
    QTRY_VERIFY(findVisibleItem(root, "conversationChangesEmpty") != nullptr);

    QSignalSpy changed(&pane, &ketplus::ConversationChangesPane::changesChanged);
    pane.setChanges("/repo", "develop", {"src/app/Main.cpp", "README.md"});
    QCOMPARE(changed.count(), 1);
    pane.setChanges("/repo", "develop", {"src/app/Main.cpp", "README.md"});
    QCOMPARE(changed.count(), 1); // the same snapshot again changes nothing
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(root, "conversationChangedFiles")) != nullptr);
    QCOMPARE(card->property("note").toString(), QStringLiteral("develop · not committed"));
    const auto files = card->property("files").toList();
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.at(0).toMap().value("name").toString(), QStringLiteral("Main.cpp"));
    QCOMPARE(files.at(0).toMap().value("path").toString(), QStringLiteral("src/app"));
    QCOMPARE(files.at(1).toMap().value("path").toString(), QString());

    QSignalSpy diff(&pane, &ketplus::ConversationChangesPane::fileDiffRequested);
    QVERIFY(QMetaObject::invokeMethod(card, "fileActivated", Q_ARG(int, 1)));
    QCOMPARE(diff.count(), 1);
    QCOMPARE(diff.first().first().toString(), QStringLiteral("/repo/README.md"));
    QSignalSpy sourceControl(&pane, &ketplus::ConversationChangesPane::sourceControlRequested);
    auto* open = findVisibleItem(root, "changesOpenSourceControl");
    QVERIFY(open != nullptr);
    QVERIFY(QMetaObject::invokeMethod(open, "clicked"));
    QCOMPARE(sourceControl.count(), 1);
    if (const auto capture = qEnvironmentVariable("KETPLUS_CHANGES_PANE_CAPTURE");
        !capture.isEmpty()) {
        QTest::qWait(120);
        QVERIFY(pane.quickView()->grabWindow().save(capture));
    }

    pane.setChanges("/repo", "develop", {});
    QTRY_VERIFY(findVisibleItem(root, "conversationChangesEmpty") != nullptr);
}

// ADR 0106, mock workspace/report-running: while a turn runs, its latest report closes the
// transcript with a Running badge and the live activity in place of the run line. The turn's
// end hands over to the end-of-turn card, and the next turn starts without the old report.
void AgentWorkspaceViewTest::runningTurnShowsItsWorkReport() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 900);
    view.show();
    view.resetMessages({QVariantMap{{"kind", "user"}, {"body", "Carry on with the dialog."}}},
                       "running-report");
    view.setSessionState("Follow up on the new UI", QStringLiteral("Working…"), true);
    view.setWorkingDetail("Editing ExecutionWorktreeDialog.cpp");
    auto* root = view.quickView()->rootObject();
    QQuickItem* indicator = nullptr;
    QTRY_VERIFY((indicator = findVisibleItem(root, "workingIndicator")) != nullptr);
    QVERIFY(findVisibleItem(root, "runningWorkReport") == nullptr);
    const qreal runLineX = indicator->mapToScene(QPointF{}).x();

    view.setTurnWorkReport(mockReport().toVariantMap());
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisibleItem(root, "runningWorkReport")) != nullptr);
    QVERIFY(findVisibleItem(root, "workingIndicator") == nullptr);
    QVERIFY(card->property("running").toBool());
    // It starts where the run line it replaces started: at the transcript's rows (T-88).
    QCOMPARE(card->mapToScene(QPointF{}).x(), runLineX);
    QCOMPARE(card->property("total").toInt(), 7);
    QCOMPARE(card->property("finished").toInt(), 3);
    // The bar follows the counts even though the report arrived after the card was made.
    QQuickItem* bar = nullptr;
    const auto findBar = [&](const auto& self, QQuickItem* item) -> void {
        if (bar == nullptr && item->inherits("QQuickProgressBar"))
            bar = item;
        for (auto* child : item->childItems())
            self(self, child);
    };
    findBar(findBar, card);
    QVERIFY(bar != nullptr);
    QVERIFY(qAbs(bar->property("position").toReal() - 3.0 / 7.0) < 0.001);
    auto* footer = findVisibleItem(card, "workReportFooter");
    QVERIFY(footer != nullptr);
    QVERIFY(!footer->property("opens").toBool());
    QCOMPARE(footer->property("label").toString(),
             QStringLiteral("Working · Editing ExecutionWorktreeDialog.cpp"));
    QVERIFY(findVisibleItem(card, "workReportActivity") != nullptr);
    if (const auto capture = qEnvironmentVariable("KETPLUS_WORK_REPORT_RUNNING_CAPTURE");
        !capture.isEmpty()) {
        card->setProperty("openItem", "picker-dialog");
        QTest::qWait(120);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }

    view.setSessionState("Follow up on the new UI", "Completed", false);
    QTRY_VERIFY(findVisibleItem(root, "runningWorkReport") == nullptr);
    view.setSessionState("Follow up on the new UI", QStringLiteral("Working…"), true);
    QVERIFY(view.turnWorkReport().isEmpty());
    QTRY_VERIFY(findVisibleItem(root, "workingIndicator") != nullptr);
    QVERIFY(findVisibleItem(root, "runningWorkReport") == nullptr);
}

// #404 "Done when": the card, the running card and the Report tab rendered from the mock's
// fixture in light and dark at 100% and 125%, for a side-by-side check with mockups/qt
// (workspace/report, workspace/report-running and workspace/report-details). Without
// KETPLUS_WORK_REPORT_PARITY_DIR it still checks that every cell lays out at its zoom.
void AgentWorkspaceViewTest::workReportCapturesMatchTheMock() {
    const QString directory = qEnvironmentVariable("KETPLUS_WORK_REPORT_PARITY_DIR");
    auto& appearance = ket::Appearance::instance();
    const QJsonObject report = mockReport();
    const auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events",
          QJsonArray{journalEvent(1, 4, "user.message",
                                  {{"text", "Carry on with the picker and the follow-ups."}}),
                     journalEvent(2, 4, "work_report.updated", report),
                     journalEvent(3, 4, "turn.completed", {{"status", "completed"}})}}});
    for (const bool dark : {false, true}) {
        for (const double zoom : {1.0, 1.25}) {
            appearance.setMode(dark ? ket::Appearance::Mode::Dark : ket::Appearance::Mode::Light);
            appearance.setZoom(zoom);
            const QString cell =
                QStringLiteral("%1-%2").arg(dark ? "dark" : "light", zoom == 1.0 ? "1" : "1.25");
            const auto save = [&](QQuickWindow* window, const QString& state) {
                if (directory.isEmpty())
                    return;
                QTest::qWait(150);
                QVERIFY(window->grabWindow().save(
                    QDir(directory).filePath(QStringLiteral("%1-%2.png").arg(state, cell))));
            };
            {
                ketplus::AgentWorkspaceView view;
                view.applyTheme(ketplus::currentThemePalette());
                view.resize(1440, 900);
                view.show();
                view.resetMessages(rows, "parity-" + cell);
                QQuickItem* card = nullptr;
                QTRY_VERIFY((card = findVisibleItem(view.quickView()->rootObject(),
                                                    "transcriptWorkReport")) != nullptr);
                QCOMPARE(card->property("finished").toInt(), 3);
                QTRY_VERIFY(card->height() > 0 && card->width() > 0);
                card->setProperty("openItem", "picker-dialog");
                save(view.quickView(), "report");
            }
            {
                ketplus::AgentWorkspaceView view;
                view.applyTheme(ketplus::currentThemePalette());
                view.resize(1440, 900);
                view.show();
                view.resetMessages(
                    {QVariantMap{{"kind", "user"}, {"body", "Carry on with the dialog."}}},
                    "parity-running-" + cell);
                view.setSessionState("Follow up on the new UI", QStringLiteral("Working…"), true);
                view.setWorkingDetail("Editing ExecutionWorktreeDialog.cpp");
                view.setTurnWorkReport(report.toVariantMap());
                QQuickItem* card = nullptr;
                QTRY_VERIFY((card = findVisibleItem(view.quickView()->rootObject(),
                                                    "runningWorkReport")) != nullptr);
                card->setProperty("openItem", "picker-dialog");
                save(view.quickView(), "report-running");
            }
            {
                ketplus::WorkReportPane pane;
                pane.resize(520, 900);
                pane.show();
                pane.showConversation("parity");
                pane.acceptReport("parity", report.toVariantMap(), {});
                QQuickItem* ledger = nullptr;
                QTRY_VERIFY((ledger = findVisibleItem(pane.quickView()->rootObject(),
                                                      "workReportLedger")) != nullptr);
                QCOMPARE(ledger->property("total").toInt(), 7);
                save(pane.quickView(), "report-details");
            }
        }
    }
    appearance.setMode(ket::Appearance::Mode::Dark);
    appearance.setZoom(1.0);
}
