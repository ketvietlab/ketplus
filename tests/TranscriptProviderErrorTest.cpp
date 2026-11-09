#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"

#include <QJsonArray>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

namespace {
QQuickItem* shownItem(QQuickItem* item, const QString& name) {
    if (!item->isVisible())
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (auto* child : item->childItems())
        if (auto* match = shownItem(child, name))
            return match;
    return nullptr;
}
} // namespace

// The mock's Codex failure: a model at capacity offers another model, not the same one again.
void AgentWorkspaceViewTest::capacityErrorOffersAnotherModel() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(900, 700);
    view.show();
    auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events",
          QJsonArray{QJsonObject{{"cursor", 1},
                                 {"turn_sequence", 1},
                                 {"event_type", "user.message"},
                                 {"payload", QJsonObject{{"text", "Profile it"}}}},
                     QJsonObject{{"cursor", 2},
                                 {"turn_sequence", 1},
                                 {"event_type", "runtime.error"},
                                 {"payload", QJsonObject{{"message", "Selected model is at "
                                                                     "capacity. Please try a "
                                                                     "different model."}}}}}}});
    view.resetMessages(rows, "capacity");
    auto* root = view.quickView()->rootObject();
    QQuickItem* action = nullptr;
    QTRY_VERIFY((action = shownItem(root, "transcriptNoticeAction")));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Switch model"));
    auto* title = shownItem(root, "transcriptNoticeTitle");
    QVERIFY(title);
    QCOMPARE(title->property("text").toString(), QStringLiteral("Selected model is at capacity"));
    auto* picker = root->property("modelPicker").value<QObject*>();
    QVERIFY(picker);
    QVERIFY(!picker->property("opened").toBool());
    QSignalSpy sent(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    QVERIFY(QMetaObject::invokeMethod(action, "clicked"));
    QTRY_VERIFY(picker->property("opened").toBool());
    QCOMPARE(sent.count(), 0);
}
