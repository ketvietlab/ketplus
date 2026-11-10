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

// The mock's workspace/error: a provider that went quiet reads "stopped responding", with
// Retry, and the failed turn's end adds no "Failed" line under the banner.
void AgentWorkspaceViewTest::silentProviderReadsAsTheMocksError() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(900, 700);
    view.show();
    auto rows = ketplus::agent_message::daemonWindowMessages(
        {{"events",
          QJsonArray{
              QJsonObject{{"cursor", 1},
                          {"turn_sequence", 1},
                          {"event_type", "user.message"},
                          {"payload", QJsonObject{{"text", "Profile the editor startup"}}}},
              QJsonObject{{"cursor", 2},
                          {"turn_sequence", 1},
                          {"event_type", "runtime.error"},
                          {"payload", QJsonObject{{"message", "Codex did not answer for 60 "
                                                              "seconds. Your message and the "
                                                              "work so far are kept."}}}},
              QJsonObject{{"cursor", 3},
                          {"turn_sequence", 1},
                          {"event_type", "turn.completed"},
                          {"payload", QJsonObject{{"status", "failed"}}}}}}});
    view.resetMessages(rows, "silent");
    auto* root = view.quickView()->rootObject();
    QQuickItem* title = nullptr;
    QTRY_VERIFY((title = shownItem(root, "transcriptNoticeTitle")));
    QCOMPARE(title->property("text").toString(), QStringLiteral("The provider stopped responding"));
    QQuickItem* action = nullptr;
    QTRY_VERIFY((action = shownItem(root, "transcriptNoticeAction")));
    QCOMPARE(action->property("text").toString(), QStringLiteral("Retry"));
    QVERIFY(!shownItem(root, "transcriptTurnEnd"));

    // Any other failure keeps its own words and its turn's end line.
    rows = ketplus::agent_message::daemonWindowMessages(
        {{"events", QJsonArray{QJsonObject{{"cursor", 1},
                                           {"turn_sequence", 1},
                                           {"event_type", "user.message"},
                                           {"payload", QJsonObject{{"text", "Profile it"}}}},
                               QJsonObject{{"cursor", 2},
                                           {"turn_sequence", 1},
                                           {"event_type", "turn.completed"},
                                           {"payload", QJsonObject{{"status", "failed"}}}}}}});
    view.resetMessages(rows, "failed-alone");
    QTRY_VERIFY(shownItem(root, "transcriptTurnEnd"));
}
