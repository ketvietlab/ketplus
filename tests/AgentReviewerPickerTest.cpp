#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"

#include <QQuickView>
#include <QScopeGuard>
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
    view.resize(1200, 700); // room for the full Reviewer label beside the product sidebar
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
    // ADR 0067: Codex is listed, as the mock, but cannot review; Claude names its model.
    auto* codex = menuItem(menu, "newCodexReviewerItem");
    QVERIFY(codex && !codex->property("enabled").toBool());
    QCOMPARE(menuItem(menu, "newClaudeReviewerItem")->property("text").toString(),
             QStringLiteral("Claude"));
    view.setModels({QVariantMap{{"provider", "claude"},
                                {"model", "claude-opus-5-5"},
                                {"displayName", "Opus 5.5"},
                                {"isDefault", true}},
                    QVariantMap{{"provider", "codex"},
                                {"model", "gpt-5.6-sol"},
                                {"displayName", "GPT-5.6 Sol"},
                                {"isDefault", true}}});
    QTRY_COMPARE(menuItem(menu, "newClaudeReviewerItem")->property("text").toString(),
                 QStringLiteral("Claude · Opus 5.5"));
    QCOMPARE(codex->property("text").toString(), QStringLiteral("Codex · GPT-5.6 Sol"));
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
    view.resize(1000, 700);
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

void AgentWorkspaceViewTest::reviewerLabelCollapsesBeforeTheTitleSqueezes_data() {
    QTest::addColumn<qreal>("zoom");
    QTest::newRow("medium") << qreal(1);
    QTest::newRow("large") << qreal(1.25);
    QTest::newRow("largest") << qreal(1.5);
}

// The header gives way in the mock's order: the Reviewer label first, then the title. The full
// "Reviewer · @agent/N" label shows only when every visible header item fits.
void AgentWorkspaceViewTest::reviewerLabelCollapsesBeforeTheTitleSqueezes() {
    QFETCH(qreal, zoom);
    auto& appearance = ket::Appearance::instance();
    const qreal previousZoom = appearance.zoom();
    appearance.setZoom(zoom);
    const auto restoreZoom = qScopeGuard([&] { appearance.setZoom(previousZoom); });
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.setProductSidebarVisible(false); // as in the shell, which has its own sidebar
    view.setConversations({conversation("a", "9410", true, true)});
    view.setSessionState("Startup profiling", "Ready", false);
    view.setExecutionBranch("feat/customer-care-routine-workflow");
    view.acceptReviewerState(
        {{"available", true},
         {"reviewer",
          QVariantMap{{"number", "1093"}, {"title", "Parser"}, {"provider", "claude"}}}});
    view.resize(1800, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    QQuickItem* button = nullptr;
    QTRY_VERIFY((button = findVisualItem(root, "headerReviewerButton")) && button->isVisible());
    auto* title = findVisualItem(root, "conversationTitle");
    auto* scope = findVisualItem(root, "workspaceScopeChip");
    QVERIFY(title && scope);
    bool sawFull = false;
    bool sawCollapsed = false;
    for (int width = 1800; width >= 760; width -= 20) {
        view.resize(width, 700);
        QTest::qWait(20);
        // The title row always ends before the header actions, collapsed or not.
        qreal actionsLeft = width;
        for (const char* name :
             {"headerCompactWindowButton", "headerAtlasButton", "headerNewConversationButton",
              "headerChangesButton", "headerTerminalButton", "headerWorkReportButton"}) {
            auto* action = findVisualItem(root, name);
            if (action && action->isVisible())
                actionsLeft = qMin(actionsLeft, action->mapToItem(root, QPointF()).x());
        }
        const auto right = button->mapToItem(root, QPointF(button->width(), 0)).x();
        QVERIFY2(right <= actionsLeft,
                 qPrintable(QStringLiteral("Reviewer ends at %1, actions start at %2 (width %3)")
                                .arg(right)
                                .arg(actionsLeft)
                                .arg(width)));
        const bool full = button->property("text").toString() != QStringLiteral("▾");
        sawCollapsed = sawCollapsed || !full;
        sawFull = sawFull || full;
        if (!full)
            continue;
        QVERIFY2(title->width() >= title->implicitWidth() - 1,
                 qPrintable(QStringLiteral("title squeezed at %1").arg(width)));
        QVERIFY2(scope->width() >= scope->implicitWidth() - 1,
                 qPrintable(QStringLiteral("scope squeezed at %1").arg(width)));
    }
    QVERIFY(sawFull);
    QVERIFY(sawCollapsed);
}
