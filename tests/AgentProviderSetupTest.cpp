#include "AgentWorkspaceViewTest.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/ProviderRuntimeStatus.h"
#include "agent/ProviderSetupGuide.h"

#include "KetAppearance.h"

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
                                                  {"login_command", "agent login"}}}}};
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

    // A stopped daemon lists no provider: the guide goes, but nothing says Cursor works.
    view.setProviderRuntime(QVariantMap{{"gated", true}});
    QTRY_VERIFY(!shownItem(root, "agentProviderSetupCard"));
    QTest::qWait(50);
    QCOMPARE(sent.count(), 0);

    // Once Cursor works, the kept message is sent.
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorReady()));
    QTRY_VERIFY(!shownItem(root, "agentProviderSetupCard"));
    QTRY_COMPARE(sent.count(), 1);
    QCOMPARE(sent.first().first().toString(), QStringLiteral("Fix the flaky test"));
    QCOMPARE(sent.first().at(1).toMap().value("provider").toString(), QStringLiteral("cursor"));
}

void AgentWorkspaceViewTest::aHeldMessageGoesWithItsDraft() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1200, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    selectProvider(root, "cursor");
    QSignalSpy sent(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    const auto hold = [&] {
        view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorMissing()));
        prompt->setProperty("text", QStringLiteral("Fix the flaky test"));
        QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
        QVERIFY(root->property("heldMessage").isValid() && !root->property("heldMessage").isNull());
    };

    // Editing the draft lets the held message go.
    hold();
    prompt->setProperty("text", QStringLiteral("Fix the flaky test, then"));
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorReady()));
    QTest::qWait(50);
    QCOMPARE(sent.count(), 0);

    // So does opening another conversation with the same provider.
    hold();
    view.setConversations({QVariantMap{{"id", "other"}, {"provider", "cursor"}, {"active", true}}});
    prompt->setProperty("text", QStringLiteral("Fix the flaky test"));
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorReady()));
    QTest::qWait(50);
    QCOMPARE(sent.count(), 0);

    // A guide that stops blocking is not a provider that works.
    hold();
    QJsonArray signedOut{QJsonObject{{"provider", "cursor"},
                                     {"name", "Cursor CLI"},
                                     {"status", "supported"},
                                     {"readiness", QJsonObject{{"state", "signed_out"}}}}};
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(signedOut));
    QVERIFY(!root->property("providerBlocked").toBool()); // Cursor's sign-out never blocks
    QTest::qWait(50);
    QCOMPARE(sent.count(), 0);
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorReady()));
    QTRY_COMPARE(sent.count(), 1);
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

// Model & context names the provider's problem; Show setup steps opens its guide in a dialog,
// which closes once the provider works.
void AgentWorkspaceViewTest::modelPickerOpensTheProviderSetupGuide() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1200, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    // A Codex model is chosen; Cursor has none until its CLI is installed.
    view.setModels({QVariantMap{{"provider", "codex"},
                                {"model", "astra"},
                                {"displayName", "GPT-6-Astra"},
                                {"defaultReasoningEffort", "medium"}}});
    view.setConversationConfiguration({{"model", "astra"}, {"effort", "medium"}});
    auto* root = view.quickView()->rootObject();
    selectProvider(root, "cursor");
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorMissing()));
    QVERIFY(QMetaObject::invokeMethod(root, "openModelPicker"));
    auto* picker = root->property("modelPicker").value<QObject*>();
    QTRY_VERIFY(picker->property("opened").toBool());
    // No Cursor models without the CLI, yet its tab stays open so the banner can guide.
    QCOMPARE(picker->property("selectedProviderTab").toString(), QStringLiteral("cursor"));
    auto* cursorTab = findVisualItem(qvariant_cast<QQuickItem*>(picker->property("contentItem")),
                                     QStringLiteral("providerTab_cursor"));
    QVERIFY(cursorTab);
    QVERIFY(cursorTab->isEnabled());
    QQuickItem* banner = nullptr;
    QTRY_VERIFY((banner = picker->findChild<QQuickItem*>("modelPickerSetupBanner")) &&
                banner->isVisible());
    QCOMPARE(banner->property("title").toString(), QStringLiteral("Cursor CLI isn't installed"));
    QCOMPARE(banner->property("message").toString(),
             QStringLiteral("Cursor's models appear once it is set up."));
    // The draft still holds the previous provider's model: Apply must not send it as Cursor's.
    auto* apply = findVisualItem(qvariant_cast<QQuickItem*>(picker->property("contentItem")),
                                 QStringLiteral("applyModelButton"));
    QVERIFY(apply);
    picker->setProperty("draftModel", QStringLiteral("astra")); // as opening from Codex leaves it
    QVERIFY(!apply->isEnabled());
    auto* empty = picker->findChild<QQuickItem*>("modelPickerEmptyState");
    QVERIFY(empty);
    QCOMPARE(empty->property("text").toString(),
             QStringLiteral("Models appear once Cursor is set up"));
    if (const auto capture = qEnvironmentVariable("KETPLUS_SETUP_CAPTURE_DIR");
        !capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture + "/model-cli-missing.png"));
    }
    QVERIFY(QMetaObject::invokeMethod(banner, "actionTriggered"));
    auto* dialog = root->findChild<QObject*>("providerSetupDialog");
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_VERIFY(!picker->property("opened").toBool());
    QCOMPARE(dialog->property("title").toString(), QStringLiteral("Cursor CLI isn't installed"));
    if (const auto capture = qEnvironmentVariable("KETPLUS_SETUP_CAPTURE_DIR");
        !capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture + "/setup-dialog-cli-missing.png"));
    }
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(cursorReady()));
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

// #170 acceptance: every #169 state (the mock's `setup-*` fixtures) in light and dark, at
// zoom 1 and 1.25. With KETPLUS_SETUP_MATRIX_DIR set, each is saved as
// <state>-<light|dark>-<100|125>.png beside the mock's `--state=setup-<state>` capture.
void AgentWorkspaceViewTest::everySetupStateRendersInBothThemesAndZooms() {
    struct Composer {
        const char* state;
        const char* title;
        QJsonObject provider;
    };
    const QList<Composer> composer{
        {"cli-missing", "Cursor CLI isn't installed", cursorMissing().first().toObject()},
        {"cli-outdated", "Codex CLI 0.139.0 is too old",
         QJsonObject{{"provider", "codex"},
                     {"name", "Codex CLI"},
                     {"status", "outdated"},
                     {"version", "0.139.0"},
                     {"minimum", "0.154.0"},
                     {"readiness",
                      QJsonObject{{"state", "outdated"},
                                  {"update_command", "npm install -g @openai/codex@latest"}}}}},
        {"signed-out", "Codex CLI isn't signed in",
         QJsonObject{{"provider", "codex"},
                     {"name", "Codex CLI"},
                     {"status", "supported"},
                     {"readiness",
                      QJsonObject{{"state", "signed_out"}, {"login_command", "codex login"}}}}},
        {"claude-signed-out", "Claude needs a sign-in",
         QJsonObject{{"provider", "claude"},
                     {"status", "bundled"},
                     {"readiness", QJsonObject{{"state", "signed_out"},
                                               {"install_command",
                                                "curl -fsSL https://claude.ai/install.sh | bash"},
                                               {"login_command", "claude"}}}}},
        {"sidecar-missing", "KetPlus can't find its Claude runtime",
         QJsonObject{{"provider", "claude"},
                     {"status", "missing"},
                     {"readiness", QJsonObject{{"state", "runtime_missing"}}}}},
    };
    struct Turn {
        const char* state;
        const char* provider;
        const char* code;
        const char* message;
        const char* title;
    };
    const QList<Turn> turns{
        {"expired", "claude", "auth_expired",
         "Failed to authenticate: OAuth session expired and could not be refreshed",
         "Your Claude sign-in expired"},
        {"key-rejected", "claude", "key_rejected", "Anthropic rejected this API key.",
         "Anthropic rejected your API key"},
        {"limit", "codex", "limit_reached", "You've hit your weekly limit",
         "You've used your Codex limit"},
        {"capacity", "codex", "model_capacity",
         "Selected model is at capacity. Please try a different model.",
         "GPT-5.6 Luna is at capacity"},
        {"plan-blocked", "cursor", "plan_blocked",
         "Cursor blocked this model for the Cursor Agent CLI account.",
         "The Cursor CLI account can't use Composer 2"},
    };
    auto& appearance = ket::Appearance::instance();
    const auto originalMode = appearance.mode();
    const qreal originalZoom = appearance.zoom();
    const auto restore = qScopeGuard([&] {
        appearance.setMode(originalMode);
        appearance.setZoom(originalZoom);
    });
    const QString directory = qEnvironmentVariable("KETPLUS_SETUP_MATRIX_DIR");
    const auto models = [](const char* provider) {
        const QString id = QString::fromLatin1(provider);
        const QString label = id == "codex"    ? QStringLiteral("GPT-5.6 Luna")
                              : id == "cursor" ? QStringLiteral("Composer 2")
                                               : QStringLiteral("Opus 5.5");
        return QVariantList{
            QVariantMap{{"id", label}, {"displayName", label}, {"label", label}, {"provider", id}}};
    };
    for (const bool dark : {false, true}) {
        for (const qreal zoom : {1.0, 1.25}) {
            appearance.setMode(dark ? ket::Appearance::Mode::Dark : ket::Appearance::Mode::Light);
            appearance.setZoom(zoom);
            const QString suffix =
                QStringLiteral("-%1-%2.png").arg(dark ? "dark" : "light").arg(qRound(zoom * 100));
            for (const Composer& state : composer) {
                ketplus::AgentWorkspaceView view;
                view.applyTheme(testPalette());
                view.setAgentAvailable(true);
                view.resize(1440, 900);
                view.show();
                QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
                auto* root = view.quickView()->rootObject();
                const QString provider = state.provider.value("provider").toString();
                view.setModels(models(qPrintable(provider)));
                selectProvider(root, provider);
                view.setProviderRuntime(
                    ketplus::ProviderRuntimeStatus::stateFrom(QJsonArray{state.provider}));
                QQuickItem* title = nullptr;
                QTRY_VERIFY((title = shownItem(root, "agentProviderSetupTitle")));
                QCOMPARE(title->property("text").toString(), QString::fromUtf8(state.title));
                if (!directory.isEmpty()) {
                    QTest::qWait(150);
                    QVERIFY(view.quickView()->grabWindow().save(directory + "/" + state.state +
                                                                suffix));
                }
            }
            for (const Turn& state : turns) {
                ketplus::AgentWorkspaceView view;
                view.applyTheme(testPalette());
                view.setAgentAvailable(true);
                view.resize(1440, 900);
                view.show();
                QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
                view.setTurnGuide([](const QString& provider, const QString& code,
                                     const QString& message, const QString& model) {
                    return ketplus::provider_setup::guide(
                        QJsonObject{{"provider", provider},
                                    {"readiness", QJsonObject{{"login_command",
                                                               provider == "cursor"  ? "agent login"
                                                               : provider == "codex" ? "codex login"
                                                                                     : "claude"}}}},
                        {{"code", code}, {"message", message}}, model);
                });
                auto* root = view.quickView()->rootObject();
                view.setModels(models(state.provider));
                selectProvider(root, state.provider);
                const auto event = [](int cursor, const QString& type, const QJsonObject& payload) {
                    return QJsonObject{{"cursor", cursor},
                                       {"turn_sequence", 1},
                                       {"event_type", type},
                                       {"payload", payload}};
                };
                const QString model = models(state.provider).first().toMap().value("id").toString();
                const QJsonArray failed{event(1, "user.message", {{"text", "Profile the sidebar"}}),
                                        event(2, "runtime.error",
                                              {{"code", state.code},
                                               {"message", state.message},
                                               {"provider", state.provider},
                                               {"model", model}})};
                view.resetMessages(
                    ketplus::agent_message::daemonWindowMessages({{"events", failed}}),
                    state.state);
                QQuickItem* title = nullptr;
                QTRY_VERIFY((title = shownItem(root, "agentProviderSetupTitle")));
                QCOMPARE(title->property("text").toString(), QString::fromUtf8(state.title));
                if (!directory.isEmpty()) {
                    QTest::qWait(150);
                    QVERIFY(view.quickView()->grabWindow().save(directory + "/" + state.state +
                                                                suffix));
                }
            }
        }
    }
}
