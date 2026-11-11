#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"

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

QObject* menuItem(QObject* menu, const QString& name) {
    for (int index = 0; index < menu->property("count").toInt(); ++index) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, index));
        if (item && item->objectName() == name)
            return item;
    }
    return nullptr;
}

} // namespace

// ADR 0114 (#467): a member's header shows its role first, locked, and has no Reviewer choice;
// leaving the team restores the choice with the setting it had.
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
    QVERIFY(!findVisualItem(root, "conversationRoleTag"));

    view.setTeamRoles({{"s-lead", leaderRole()}});
    QTRY_VERIFY(!reviewer->isVisible());
    QQuickItem* role = nullptr;
    QTRY_VERIFY((role = findVisualItem(root, "conversationRoleTag")));
    QVERIFY(role->property("locked").toBool());
    QCOMPARE(role->property("text").toString(), QStringLiteral("Leader"));
    // The role comes first, before the user's own tag of the same name.
    auto* user = findVisualItem(root, "conversationTag_Leader");
    QVERIFY(user && user->isVisible());
    QTRY_VERIFY(role->x() < user->x()); // siblings in the header's tag row
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::teamRequested);
    auto* area = role->findChild<QQuickItem*>("conversationRoleTagArea");
    QVERIFY(area);
    QTest::mouseClick(view.quickView(), Qt::LeftButton, {},
                      area->mapToScene(QPointF(area->width() / 2, area->height() / 2)).toPoint());
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.first().first().toString(), QStringLiteral("7"));

    // The tag menu starts with the role, disabled; the user's "Leader" stays a user tag.
    auto* addTag = findVisualItem(root, "headerAddTagButton");
    QVERIFY(addTag && addTag->isVisible());
    QVERIFY(QMetaObject::invokeMethod(addTag, "clicked"));
    auto* tagMenu = root->findChild<QObject*>("conversationTagMenu");
    QTRY_VERIFY(tagMenu->property("visible").toBool());
    QQuickItem* first = nullptr;
    QMetaObject::invokeMethod(tagMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, first), Q_ARG(int, 0));
    QCOMPARE(first->objectName(), QStringLiteral("conversationRoleTagItem"));
    QVERIFY(!first->isEnabled());
    QCOMPARE(first->property("text").toString(), QStringLiteral("Leader · Set by Shop"));
    // The role, the catalog, then the separator and Manage tags….
    QCOMPARE(tagMenu->property("count").toInt(), view.tagCatalog().size() + 3);
    // A catalog that changes while the conversation is a member stays below the role.
    QVariantList catalog = view.tagCatalog();
    catalog.append(QVariantMap{{"label", "Later"}, {"color", "#5b6b7f"}});
    view.setTagCatalog(catalog);
    QTRY_COMPARE(tagMenu->property("count").toInt(), catalog.size() + 3);
    QMetaObject::invokeMethod(tagMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, first), Q_ARG(int, 0));
    QCOMPARE(first->objectName(), QStringLiteral("conversationRoleTagItem"));
    QVERIFY(QMetaObject::invokeMethod(tagMenu, "close"));
    QTRY_VERIFY(!tagMenu->property("visible").toBool());

    // Leaving the team: the role goes, the Reviewer choice returns with its reviewer.
    view.setTeamRoles({});
    QTRY_VERIFY(reviewer->isVisible());
    QVERIFY(reviewer->property("text").toString().contains("@agent/1093"));
    QTRY_VERIFY(!findVisualItem(root, "conversationRoleTag"));
    QVERIFY(!menuItem(tagMenu, "conversationRoleTagItem"));
    QCOMPARE(tagMenu->property("count").toInt(), view.tagCatalog().size() + 2);
}
