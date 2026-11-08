#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"

#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QVariantMap conversation(const QString& id, const QString& number, const bool active,
                         const bool started) {
    return {{"id", id},
            {"number", number},
            {"title", "Task " + number},
            {"provider", "codex"},
            {"active", active},
            {"started", started},
            {"status", "Ready"}};
}

QObject* menuItem(QObject* menu, const QString& name) {
    const int count = menu->property("count").toInt();
    for (int index = 0; index < count; ++index) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, index));
        if (item && item->objectName() == name)
            return item;
    }
    return nullptr;
}

} // namespace

// ADR 0067: the header Reviewer button and menu, as in mockups/qt ConversationHeader.
void AgentWorkspaceViewTest::reviewerPickerFollowsTheActiveConversation() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.setConversationDirectory([] {
        return QVariantList{
            QVariantMap{{"number", "9410"}, {"title", "Working"}, {"provider", "codex"}},
            QVariantMap{{"number", "1093"},
                        {"title", "Parser"},
                        {"provider", "claude"},
                        {"path", "/work/parser"}},
            QVariantMap{{"number", "2044"}, {"title", "Cursor"}, {"provider", "cursor"}},
            QVariantMap{{"number", "3055"}, {"title", "Codex"}, {"provider", "codex"}}};
    });
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    QSignalSpy reads(&view, &ketplus::AgentWorkspaceView::reviewerRequested);
    QSignalSpy changes(&view, &ketplus::AgentWorkspaceView::reviewerChangeRequested);
    auto* root = view.quickView()->rootObject();
    QQuickItem* button = nullptr;
    QTRY_VERIFY((button = findVisualItem(root, "headerReviewerButton")));

    // A conversation without a first turn has nothing to review.
    view.setConversations({conversation("a", "9410", true, false)});
    QCOMPARE(reads.size(), 0);
    QVERIFY(!button->isVisible());
    view.setConversations({conversation("a", "9410", true, true)});
    QCOMPARE(reads.size(), 1);
    view.setConversations({conversation("a", "9410", true, true)});
    QCOMPARE(reads.size(), 1);
    QVERIFY(!button->isVisible());
    // A daemon without reviewers keeps the picker hidden.
    view.acceptReviewerState({{"error", "Update the daemon to choose a reviewer."}});
    QVERIFY(!button->isVisible());
    view.acceptReviewerState({{"available", true}});
    QTRY_VERIFY(button->isVisible());
    QCOMPARE(button->property("text").toString(), QStringLiteral("Reviewer  ▾"));

    // Only other Claude conversations can review.
    const auto candidates = view.reviewerCandidates();
    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().toMap().value("number").toString(), QStringLiteral("1093"));
    QCOMPARE(candidates.first().toMap().value("worktree").toString(), QStringLiteral("parser"));

    auto* header = findVisualItem(root, "agentWorkspaceHeader");
    QVERIFY(header);
    QVERIFY(QMetaObject::invokeMethod(header, "openReviewerMenu"));
    auto* menu = header->findChild<QObject*>("reviewerMenu");
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(menuItem(menu, "noReviewerItem")->property("checked").toBool());
    auto* existing = menuItem(menu, "existingReviewerItem_1093");
    QVERIFY(existing);
    QVERIFY(!menuItem(menu, "existingReviewerItem_2044"));
    QVERIFY(!menuItem(menu, "existingReviewerItem_3055"));
    QVERIFY(!menuItem(menu, "newCodexReviewerItem"));
    QVERIFY(QMetaObject::invokeMethod(existing, "triggered"));
    QCOMPARE(changes.size(), 1);
    QCOMPARE(changes.first().at(0).toString(), QStringLiteral("1093"));
    QCOMPARE(changes.first().at(1).toString(), QString{});
    QVERIFY(!button->isEnabled());
    view.chooseReviewer({}, QStringLiteral("claude")); // one change at a time
    QCOMPARE(changes.size(), 1);

    view.acceptReviewerState(
        {{"available", true},
         {"reviewer",
          QVariantMap{{"number", "1093"}, {"title", "Parser"}, {"provider", "claude"}}}});
    QTRY_VERIFY(button->isEnabled());
    QCOMPARE(button->property("text").toString(), QStringLiteral("Reviewer · @agent/1093  ▾"));
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(!menu->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(header, "openReviewerMenu"));
    QTRY_VERIFY(menu->property("visible").toBool());
    QVERIFY(!menuItem(menu, "noReviewerItem")->property("checked").toBool());
    QVERIFY(menuItem(menu, "existingReviewerItem_1093")->property("checked").toBool());

    // A new review agent; a failure keeps the reviewer that is still assigned.
    QVERIFY(QMetaObject::invokeMethod(menuItem(menu, "newClaudeReviewerItem"), "triggered"));
    QCOMPARE(changes.size(), 2);
    QCOMPARE(changes.last().at(1).toString(), QStringLiteral("claude"));
    view.acceptReviewerState({{"error", "Update the daemon to choose a reviewer."}});
    QTRY_VERIFY(button->isEnabled());
    QCOMPARE(button->property("text").toString(), QStringLiteral("Reviewer · @agent/1093  ▾"));
    QCOMPARE(view.reviewerState().value("error").toString(),
             QStringLiteral("Update the daemon to choose a reviewer."));
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));

    // Narrow headers keep only the chevron; the tooltip still names the reviewer.
    view.resize(900, 700);
    QTRY_COMPARE(button->property("text").toString(), QStringLiteral("▾"));
    auto* title = findVisualItem(header, "conversationTitle");
    QVERIFY(title);
    QVERIFY(title->mapToItem(header, QPointF(title->width(), 0)).x() <=
            button->mapToItem(header, QPointF(0, 0)).x());
    QVERIFY(button->mapToItem(header, QPointF(button->width(), 0)).x() <= header->width());
    // The Reviewer label collapses first, so the title and scope chip keep their text.
    QVERIFY(title->width() >= title->implicitWidth() - 1);
    auto* scope = findVisualItem(header, "workspaceScopeChip");
    QVERIFY(scope);
    QVERIFY(scope->width() >= scope->implicitWidth() - 1);

    // Another conversation reads its own reviewer; unstarted ones show none.
    view.setConversations(
        {conversation("a", "9410", false, true), conversation("b", "9411", true, false)});
    QTRY_VERIFY(!button->isVisible());
    QCOMPARE(reads.size(), 1);
    view.acceptReviewerState({{"available", true}}); // stale reply for the old scope
    QVERIFY(!button->isVisible());
    view.setConversations(
        {conversation("a", "9410", true, true), conversation("b", "9411", false, false)});
    QCOMPARE(reads.size(), 2);
}
