#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"

#include <QAccessible>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

QVariantMap conversation(const QString& session, const QVariantList& tags = {}) {
    return {{"id", "c-" + session}, {"sessionId", session}, {"number", "42"},  {"title", "Cart"},
            {"provider", "codex"},  {"active", true},       {"started", true}, {"status", "Ready"},
            {"tagsEditable", true}, {"tags", tags}};
}

QVariantMap leaderRole() {
    return {{"team_id", "7"},
            {"team_name", "Shop"},
            {"role", "leader"},
            {"label", "Leader"},
            {"color", "#b4436c"}};
}

} // namespace

// ADR 0114 (#467): a member's header shows its role, locked, and has no Reviewer choice;
// leaving the team restores the choice with the setting it had. ADR 0117: the header draws
// no user tag, even when a row still carries one, and offers no way to add or manage tags.
void AgentWorkspaceViewTest::teamMemberHeaderShowsItsRoleInsteadOfReviewer() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.resize(1200, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    view.setConversations(
        {conversation("s-lead", {QVariantMap{{"label", "Leader"}, {"color", "#3377cc"}}})});
    view.acceptReviewerState(
        {{"available", true}, {"reviewer", QVariantMap{{"number", "1093"}, {"title", "Parser"}}}});
    QQuickItem* reviewer = nullptr;
    QTRY_VERIFY((reviewer = findVisualItem(root, "headerReviewerButton")));
    QTRY_VERIFY(reviewer->isVisible());
    QQuickItem* role = findVisualItem(root, "conversationRoleTag");
    QVERIFY(role && !role->isVisible());

    view.setTeamRoles({{"s-lead", leaderRole()}});
    QTRY_VERIFY(!reviewer->isVisible());
    QTRY_VERIFY(role->isVisible());
    QVERIFY(role->property("locked").toBool());
    QVERIFY(!role->property("filterable").toBool());
    QCOMPARE(role->property("text").toString(), QStringLiteral("Leader"));
    QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(role);
    QVERIFY(accessible);
    QCOMPARE(accessible->text(QAccessible::Description),
             QStringLiteral("Leader of Shop. Opens the team."));
    // No user tag, Add tag, tag menu or Manage tags… remains.
    for (const char* name : {"conversationTag_Leader", "headerAddTagButton"})
        QVERIFY2(!findVisualItem(root, name), name);
    for (const char* name : {"conversationTagMenu", "manageTagsDialog"})
        QVERIFY2(!root->findChild<QObject*>(name), name);
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::teamRequested);
    auto* area = role->findChild<QQuickItem*>("conversationRoleTagArea");
    QVERIFY(area);
    QTest::mouseClick(view.quickView(), Qt::LeftButton, {},
                      area->mapToScene(QPointF(area->width() / 2, area->height() / 2)).toPoint());
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.first().first().toString(), QStringLiteral("7"));
    // A click opens the team and changes nothing else: the role stays as the team set it.
    QCOMPARE(role->property("text").toString(), QStringLiteral("Leader"));

    // Leaving the team: the role goes, the Reviewer choice returns with its reviewer.
    view.setTeamRoles({});
    QTRY_VERIFY(reviewer->isVisible());
    QVERIFY(reviewer->property("text").toString().contains("@agent/1093"));
    QTRY_VERIFY(!role->isVisible());
}
