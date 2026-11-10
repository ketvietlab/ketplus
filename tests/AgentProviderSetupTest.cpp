#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/ProviderRuntimeStatus.h"
#include "agent/ProviderSetupGuide.h"

#include <QJsonArray>
#include <QQuickView>
#include <QSignalSpy>
#include <QtTest>

using namespace agent_workspace_test;

// ADR 0110 (#170): the provider setup guide above the composer and at the end of a failed turn.
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

QJsonArray cursorMissing() {
    return {QJsonObject{{"provider", "cursor"},
                        {"name", "Cursor CLI"},
                        {"status", "missing"},
                        {"readiness", QJsonObject{{"state", "missing"},
                                                  {"install_command",
                                                   "curl https://cursor.com/install -fsS | bash"},
                                                  {"login_command", "cursor-agent login"}}}}};
}

QJsonArray cursorReady() {
    return {QJsonObject{{"provider", "cursor"},
                        {"name", "Cursor CLI"},
                        {"status", "supported"},
                        {"readiness", QJsonObject{{"state", "ready"}}}}};
}

void selectProvider(QQuickItem* root, const QString& provider) {
    root->setProperty("selectedProvider", provider);
}
} // namespace

void AgentWorkspaceViewTest::providerSetupHoldsTheMessageUntilTheProviderWorks() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1200, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    selectProvider(root, "cursor");
    QVERIFY(!shownItem(root, "composerProviderSetup"));

    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorMissing()));
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = shownItem(root, "agentProviderSetupCard")));
    QCOMPARE(shownItem(root, "agentProviderSetupTitle")->property("text").toString(),
             QStringLiteral("Cursor CLI isn't installed"));
    QVERIFY(shownItem(root, "agentProviderSetupCommand"));
    QVERIFY(shownItem(root, "modelButtonSetupWarning"));
    QVERIFY(root->property("providerSetupShown").toBool());
    QVERIFY(shownItem(root, "composerPlaceholder")
                ->property("text")
                .toString()
                .contains(QStringLiteral("it sends once Cursor is set up")));
    if (const auto capture = qEnvironmentVariable("KETPLUS_SETUP_CAPTURE_DIR"); !capture.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capture + "/composer-cli-missing.png"));

    // Type in Terminal hands the command over; KetPlus never runs it.
    QSignalSpy typed(&view, &ketplus::AgentWorkspaceView::typeInTerminalRequested);
    QVERIFY(
        QMetaObject::invokeMethod(shownItem(root, "agentProviderSetupTypeInTerminal"), "clicked"));
    QCOMPARE(typed.count(), 1);
    QCOMPARE(typed.first().first().toString(),
             QStringLiteral("curl https://cursor.com/install -fsS | bash"));

    // A blocking guide keeps the message.
    QSignalSpy sent(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    prompt->setProperty("text", QStringLiteral("Fix the flaky test"));
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QCOMPARE(sent.count(), 0);
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("Fix the flaky test"));

    // Hide, then the model button opens the guide again.
    QVERIFY(QMetaObject::invokeMethod(shownItem(root, "agentProviderSetupHide"), "clicked"));
    QTRY_VERIFY(!shownItem(root, "agentProviderSetupCard"));
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QQuickItem*>("modelButton"), "clicked"));
    QTRY_VERIFY(shownItem(root, "agentProviderSetupCard"));

    // Check again asks the daemon to probe again; the same guide says what to try next.
    QSignalSpy rechecks(&view, &ketplus::AgentWorkspaceView::providerRecheckRequested);
    auto* primary = shownItem(root, "agentProviderSetupPrimary");
    QCOMPARE(primary->property("text").toString(), QStringLiteral("Check again"));
    QVERIFY(QMetaObject::invokeMethod(primary, "clicked"));
    QCOMPARE(rechecks.count(), 1);
    QCOMPARE(primary->property("text").toString(), QStringLiteral("Checking…"));
    QVariantMap again = ketplus::ProviderRuntimeStatus::stateFrom(cursorMissing());
    again.insert("revision", 2); // a fresh answer with the same guide
    view.setProviderRuntime(again);
    QTRY_VERIFY(shownItem(root, "agentProviderSetupStill"));

    // Once Cursor works, the guide goes and the kept message is sent.
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorReady()));
    QTRY_VERIFY(!shownItem(root, "agentProviderSetupCard"));
    QTRY_COMPARE(sent.count(), 1);
    QCOMPARE(sent.first().first().toString(), QStringLiteral("Fix the flaky test"));
    QCOMPARE(sent.first().at(1).toMap().value("provider").toString(), QStringLiteral("cursor"));
}

void AgentWorkspaceViewTest::providerSetupClosesTheTurnTheProviderEnded() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1200, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    view.setTurnGuide([](const QString& provider, const QString& code, const QString& message,
                         const QString& model) {
        return ketplus::provider_setup::guide(
            QJsonObject{{"provider", provider},
                        {"readiness", QJsonObject{{"login_command", "claude"}}}},
            {{"code", code}, {"message", message}}, model);
    });
    auto* root = view.quickView()->rootObject();
    selectProvider(root, "claude");
    const auto event = [](int cursor, const QString& type, const QJsonObject& payload) {
        return QJsonObject{
            {"cursor", cursor}, {"turn_sequence", 1}, {"event_type", type}, {"payload", payload}};
    };
    const QJsonArray failed{event(1, "user.message", {{"text", "Profile it"}}),
                            event(2, "runtime.error",
                                  {{"code", "auth_expired"},
                                   {"message", "Failed to authenticate: OAuth session expired"}})};
    view.resetMessages(ketplus::agent_message::daemonWindowMessages({{"events", failed}}),
                       "expired");
    QTRY_VERIFY(shownItem(root, "transcriptProviderSetup"));
    QCOMPARE(shownItem(root, "agentProviderSetupTitle")->property("text").toString(),
             QStringLiteral("Your Claude sign-in expired"));
    // The provider's own words, not a summary.
    QCOMPARE(shownItem(root, "agentProviderSetupError")->property("text").toString(),
             QStringLiteral("Failed to authenticate: OAuth session expired"));
    QVERIFY(!shownItem(root, "transcriptNoticeTitle"));
    // Readiness reports the expired sign-in too: the model button warns, and the guide stays
    // with the turn instead of sitting above the composer.
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(QJsonArray{QJsonObject{
        {"provider", "claude"},
        {"status", "bundled"},
        {"readiness", QJsonObject{{"state", "expired"}, {"message", "OAuth session expired"}}}}}));
    QTRY_VERIFY(shownItem(root, "modelButtonSetupWarning"));
    QVERIFY(!shownItem(root, "composerProviderSetup"));
    QVERIFY(!root->property("providerBlocked").toBool());
    if (const auto capture = qEnvironmentVariable("KETPLUS_SETUP_CAPTURE_DIR");
        !capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture + "/transcript-expired.png"));
    }

    // Retry sends the failed turn's message again.
    auto* retry = shownItem(root, "agentProviderSetupPrimary");
    QCOMPARE(retry->property("text").toString(), QStringLiteral("Retry"));
    QSignalSpy sent(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
    QTRY_COMPARE(sent.count(), 1);
    QCOMPARE(sent.first().first().toString(), QStringLiteral("Profile it"));

    // Once the user writes again, the old failure reads as the notice it was.
    QJsonArray later = failed;
    later.append(event(3, "user.message", {{"text", "Try again"}}));
    view.resetMessages(ketplus::agent_message::daemonWindowMessages({{"events", later}}),
                       "expired");
    QTRY_VERIFY(!shownItem(root, "transcriptProviderSetup"));
    QTRY_VERIFY(shownItem(root, "transcriptNoticeTitle"));
}

void AgentWorkspaceViewTest::claudeSignOutGuidesWithoutHoldingTheMessage() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1200, 900);
    view.show();
    auto* root = view.quickView()->rootObject();
    selectProvider(root, "claude");
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(QJsonArray{QJsonObject{
        {"provider", "claude"},
        {"status", "bundled"},
        {"readiness", QJsonObject{{"state", "signed_out"}, {"login_command", "claude"}}}}}));
    QTRY_VERIFY(shownItem(root, "agentProviderSetupCard"));
    // Only a Claude query finds a new sign-in, so the message goes through.
    QSignalSpy sent(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    prompt->setProperty("text", QStringLiteral("Hello"));
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QCOMPARE(sent.count(), 1);
}
