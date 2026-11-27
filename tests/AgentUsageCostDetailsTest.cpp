#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"
#include "ui/Theme.h"

#include <QDir>
#include <QJsonObject>
#include <QQuickView>
#include <QScopeGuard>
#include <QtTest>

using namespace agent_workspace_test;

namespace {

const QStringList costRows{"claudeSessionCost",   "claudeSessionTokens",  "claudeCacheTokens",
                           "claudeQueryBudget",   "claudeAccountSummary", "claudeAccountRefresh",
                           "claudeAccountConsole"};

QVariantMap claudeSnapshot(const QString& mode) {
    QVariantMap session{{"estimatedCostUsd", 12.5}, {"inputTokens", 300}, {"outputTokens", 40}};
    if (!mode.isEmpty())
        session.insert("mode", mode);
    return {{"provider", "claude"}, {"session", session}};
}

struct UsagePopover {
    ketplus::AgentWorkspaceView view;
    QObject* popup = nullptr;
    QQuickItem* content = nullptr;
    int reads = 0;

    bool open(const QVariantMap& snapshot, QSize size) {
        view.applyTheme(testPalette());
        view.setAgentAvailable(true);
        view.setConversations(
            {QVariantMap{{"id", "claude"}, {"provider", "claude"}, {"active", true}}});
        view.setUsageSnapshot(snapshot);
        view.setClaudeUsageRequester([this](ketplus::AgentWorkspaceView::UsageReply reply) {
            ++reads;
            reply({{"configured", true},
                   {"report", QJsonObject{{"costUsd", 42.5}, {"totalTokens", 50000}}}},
                  {});
        });
        view.resize(size);
        view.show();
        if (!QTest::qWaitForWindowExposed(view.quickView()))
            return false;
        view.quickView()->requestActivate();
        if (!QTest::qWaitForWindowActive(view.quickView()))
            return false;
        popup = view.quickView()->rootObject()->findChild<QObject*>("usageLimitsPopover");
        if (!popup || !QMetaObject::invokeMethod(popup, "toggleFromAnchor"))
            return false;
        if (!QTest::qWaitFor([this] { return popup->property("visible").toBool(); }))
            return false;
        content = qvariant_cast<QQuickItem*>(popup->property("contentItem"));
        return content;
    }
    QQuickItem* shown(const QString& name) const { return findVisibleItem(content, name); }
    qreal height() const { return popup->property("height").toDouble(); }
    // With KETPLUS_USAGE_CAPTURE set to a folder, saves the window in dark and light as
    // review evidence beside the mock.
    void capture(const QString& name) {
        const auto folder = qEnvironmentVariable("KETPLUS_USAGE_CAPTURE");
        if (folder.isEmpty())
            return;
        QTest::qWait(150);
        view.quickView()->grabWindow().save(QDir(folder).filePath(name + "-dark.png"));
        ketplus::ThemeManager theme(*qApp);
        theme.previewMode(ketplus::ThemeManager::Mode::Light);
        view.applyTheme(theme.palette());
        QTest::qWait(250);
        view.quickView()->grabWindow().save(QDir(folder).filePath(name + "-light.png"));
        view.applyTheme(testPalette());
        QTest::qWait(150);
    }
    // The popover's height once its layout has stopped changing.
    qreal settledHeight() const {
        qreal last = -1;
        for (int i = 0; i < 20 && height() != last; ++i) {
            last = height();
            QTest::qWait(50);
        }
        return height();
    }
};

} // namespace

void AgentWorkspaceViewTest::runtimeUsageFoldsCostDetails_data() {
    QTest::addColumn<qreal>("zoom");
    QTest::newRow("100%") << qreal(1.0);
    QTest::newRow("150%") << qreal(1.5);
}

// Without an API key the conversation and organization figures wait behind one closed
// "Cost details" row; the context window and plan limits stay in view.
void AgentWorkspaceViewTest::runtimeUsageFoldsCostDetails() {
    QFETCH(qreal, zoom);
    auto& appearance = ket::Appearance::instance();
    const qreal previousZoom = appearance.zoom();
    appearance.setZoom(zoom);
    const auto restoreZoom = qScopeGuard([&] { appearance.setZoom(previousZoom); });

    for (const QString mode : {QStringLiteral("runtime"), QString()}) {
        UsagePopover usage;
        QVERIFY(usage.open(claudeSnapshot(mode), QSize(900, 480)));
        auto* row = usage.shown("claudeCostDetails");
        QVERIFY2(row, qPrintable("mode: " + mode));
        QCOMPARE(usage.popup->property("costExpanded").toBool(), false);
        for (const auto& name : costRows)
            QVERIFY2(!usage.shown(name), qPrintable(name + " is hidden while folded"));
        QCOMPARE(usage.reads, 0); // folded, opening reads no organization usage
        const qreal folded = usage.settledHeight();
        if (zoom == 1.0 && mode == "runtime")
            usage.capture("usage-runtime-folded");

        // Space on the focused row opens it: the same blocks, values and one read.
        row->forceActiveFocus();
        QTRY_VERIFY(row->hasActiveFocus());
        QTest::keyClick(usage.view.quickView(), Qt::Key_Space);
        QTRY_VERIFY(usage.shown("claudeSessionCost"));
        QCOMPARE(usage.shown("claudeSessionCost")->property("text").toString(), "$12.5000");
        for (const auto& name : costRows)
            QVERIFY2(usage.shown(name), qPrintable(name + " shows once expanded"));
        QCOMPARE(usage.popup->property("costExpanded").toBool(), true);
        QTRY_COMPARE(usage.reads, 1);
        QTRY_VERIFY(
            usage.shown("claudeAccountSummary")->property("text").toString().contains("$42.5000"));
        QTRY_VERIFY(usage.height() > folded);
        if (zoom == 1.0 && mode == "runtime")
            usage.capture("usage-runtime-expanded");
        // Taller than the window: the popover stays inside it and its content scrolls.
        QTRY_VERIFY(usage.popup->property("y").toDouble() + usage.height() <= usage.view.height());
        QVERIFY(usage.content->property("contentHeight").toDouble() >= usage.content->height());

        // Refresh reads again, as before.
        QVERIFY(QMetaObject::invokeMethod(usage.shown("claudeAccountRefresh"), "clicked"));
        QTRY_COMPARE(usage.reads, 2);

        // Enter folds it again, and the height comes back.
        QTest::keyClick(usage.view.quickView(), Qt::Key_Return);
        QTRY_VERIFY(!usage.shown("claudeSessionCost"));
        QTRY_COMPARE(usage.height(), folded);

        // Expanded when closed, it opens folded the next time.
        QTest::keyClick(usage.view.quickView(), Qt::Key_Space);
        QTRY_VERIFY(usage.shown("claudeSessionCost"));
        QVERIFY(QMetaObject::invokeMethod(usage.popup, "close"));
        QTRY_VERIFY(!usage.popup->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(usage.popup, "open"));
        QTRY_VERIFY(usage.popup->property("visible").toBool());
        QTRY_VERIFY(usage.shown("claudeCostDetails"));
        QVERIFY(!usage.shown("claudeSessionCost"));
        QCOMPARE(usage.reads, 3); // the expand before closing; reopening folded reads nothing
    }
}

// With an API key the figures are what the user pays: no row, both blocks shown, and opening
// reads the organization usage as it always did.
void AgentWorkspaceViewTest::apiKeyUsageShowsCostDetailsOpen() {
    UsagePopover usage;
    QVERIFY(usage.open(claudeSnapshot("api_key"), QSize(900, 680)));
    QVERIFY(!usage.shown("claudeCostDetails"));
    for (const auto& name : costRows)
        QVERIFY2(usage.shown(name), qPrintable(name + " shows with an API key"));
    QTRY_COMPARE(usage.reads, 1);
    usage.capture("usage-api-key");

    // Opened before any report, folded, it reads nothing; the first report naming an API key
    // drops the row, shows both blocks and reads the organization usage once.
    UsagePopover early;
    QVERIFY(early.open(claudeSnapshot(QString()), QSize(900, 680)));
    QVERIFY(early.shown("claudeCostDetails"));
    QCOMPARE(early.reads, 0);
    early.view.setUsageSnapshot(claudeSnapshot("api_key"));
    QTRY_VERIFY(!early.shown("claudeCostDetails"));
    for (const auto& name : costRows)
        QTRY_VERIFY2(early.shown(name), qPrintable(name + " shows once the API key is known"));
    QTRY_COMPARE(early.reads, 1);
    QTest::qWait(100);
    QCOMPARE(early.reads, 1);
}
