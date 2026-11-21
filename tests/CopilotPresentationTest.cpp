#include "KetAppearance.h"
#include "KetSelect.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "agent/CopilotSetupGuide.h"
#include "agent/DaemonTranscriptNotices.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/ProviderRuntimeStatus.h"
#include "agent/ProviderSetupGuide.h"
#include "teams/TeamAgentDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QJsonArray>
#include <QQuickItem>
#include <QQuickView>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus;

namespace {
QQuickItem* visual(QQuickItem* item, const QString& name) {
    if (!item)
        return nullptr;
    if (item->objectName() == name)
        return item;
    for (auto* child : item->childItems())
        if (auto* found = visual(child, name))
            return found;
    return nullptr;
}
QVariantMap model(const QString& provider) {
    return {{"provider", provider},
            {"model", "default"},
            {"displayName", "Default"},
            {"permissions", QVariantList{QVariantMap{{"id", "workspace-write"}, {"label", "Ask"}}}},
            {"defaultPermission", "workspace-write"},
            {"isDefault", true}};
}
QJsonObject runtime(const QString& state) {
    return {{"provider", "copilot"},
            {"name", "GitHub Copilot CLI"},
            {"readiness", QJsonObject{{"state", state}}}};
}
} // namespace

class CopilotPresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    void platformSetupGuides_data() {
        QTest::addColumn<QString>("platform");
        QTest::addColumn<QString>("install");
        QTest::newRow("macOS") << "macos" << "brew install --cask copilot-cli";
        QTest::newRow("Linux") << "linux" << "npm install -g @github/copilot";
        QTest::newRow("Windows") << "windows" << "winget install GitHub.Copilot";
    }
    void platformSetupGuides() {
        QFETCH(QString, platform);
        QFETCH(QString, install);
        const auto missing = provider_setup::copilotGuide(runtime("missing"), {}, platform);
        QCOMPARE(missing.value("name").toString(), QStringLiteral("GitHub Copilot"));
        QVERIFY(missing.value("blocking").toBool());
        QCOMPARE(missing.value("steps").toList().first().toMap().value("command").toString(),
                 install);
        QVERIFY(missing.value("reason").toString().contains("machine running the daemon"));
        const auto signedOut = provider_setup::copilotGuide(runtime("signed_out"), {}, platform);
        QVERIFY(!signedOut.value("blocking").toBool()); // Credentials can change outside KetPlus.
        QCOMPARE(signedOut.value("steps").toList().first().toMap().value("command").toString(),
                 QStringLiteral("copilot login"));
        QVERIFY(provider_setup::guide(runtime("ready")).isEmpty());
        QCOMPARE(provider_setup::guide(runtime("signed_out")).value("provider").toString(),
                 QStringLiteral("copilot"));
        const auto fromTurn = provider_setup::guide(
            runtime("ready"), {{"code", "auth_required"}, {"message", "Sign in"}});
        QVERIFY(!fromTurn.value("composer").toBool());
        QCOMPARE(fromTurn.value("title").toString(),
                 QStringLiteral("GitHub Copilot CLI needs sign-in"));
    }

    void daemonReportedInstallCommandWinsForARemoteHost() {
        auto remote = runtime("missing");
        remote.insert("readiness",
                      QJsonObject{{"state", "missing"},
                                  {"install_command", "npm install -g @github/copilot"}});
        QCOMPARE(provider_setup::copilotGuide(remote, {}, "macos")
                     .value("steps")
                     .toList()
                     .first()
                     .toMap()
                     .value("command")
                     .toString(),
                 QStringLiteral("npm install -g @github/copilot"));
    }
    void daemonDiscoveryAndPresentationKeepProviderIdentity() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        CodexAppServer server("/not-an-installed-provider");
        AgentSessionManager manager(&server);
        emit server.daemonSessionDiscovered(
            {{"session_id", "copilot-session"},
             {"session_generation", 1},
             {"worktree_path", directory.path()},
             {"state", "ready"},
             {"adapter", "copilot"},
             {"effective", QJsonObject{{"provider", "copilot"},
                                       {"model", "default"},
                                       {"permission", "workspace-write"}}}});
        QCOMPARE(manager.conversationConfiguration(directory.path()).value("provider").toString(),
                 QStringLiteral("copilot"));
        QCOMPARE(
            manager.conversations(directory.path()).first().toMap().value("provider").toString(),
            QStringLiteral("copilot"));
        QVariantMap row{{"meta", "Codex · answer"}};
        agent_message::labelProvider(row, "copilot");
        QCOMPARE(row.value("meta").toString(), QStringLiteral("GitHub Copilot · answer"));
        const auto notice = agent_message::daemonEventNotice(
            "session.provider_changed", {{"from", "cursor"}, {"to", "copilot"}}, "notice", {}, {});
        QVERIFY(notice.value("title").toString().contains("GitHub Copilot"));
        QVERIFY(CodexAppServer::permissionProfilesFor("copilot").isEmpty());
        QSignalSpy errors(&manager, &AgentSessionManager::errorOccurred);
        manager.submitPrompt(directory.path(), "Must not run in direct Codex mode",
                             {{"provider", "copilot"}, {"model", "default"}});
        QCOMPARE(errors.size(), 1);
        QVERIFY(!manager.isRunning(directory.path()));
    }

    void transcriptReplayPreservesCopilotErrorIdentity() {
        const auto rows = agent_message::daemonWindowMessages(
            {{"events", QJsonArray{QJsonObject{
                            {"cursor", 1},
                            {"turn_sequence", 1},
                            {"event_type", "runtime.error"},
                            {"payload", QJsonObject{{"provider", "copilot"},
                                                    {"code", "auth_required"},
                                                    {"message", "Authentication required"}}}}}}});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("provider").toString(), QStringLiteral("copilot"));
        QCOMPARE(rows.first().toMap().value("errorCode").toString(),
                 QStringLiteral("auth_required"));
    }
    void teamsOfferOnlyRunnableCopilotModels() {
        const auto copilot = model("copilot");
        const auto claude = model("claude");
        QVariantMap unavailable = copilot;
        unavailable.remove("permissions");
        QVERIFY(!team_agents::readyProviders({unavailable, claude}).contains("copilot"));
        const QVariantList catalog{copilot, claude};
        QVERIFY(team_agents::readyProviders(catalog).contains("copilot"));
        const auto defaults = team_agents::defaults(catalog);
        QCOMPARE(defaults.developer.provider, QStringLiteral("copilot"));
        QVERIFY(team_agents::label(catalog, defaults.developer).startsWith("GitHub Copilot"));
        QCOMPARE(defaults.developer.json().value("provider").toString(), QStringLiteral("copilot"));
        TeamAgentChooser chooser(catalog, defaults.developer);
        QCOMPARE(chooser.agent().provider, QStringLiteral("copilot"));
        QCOMPARE(chooser.providers()->currentText(), QStringLiteral("GitHub Copilot"));
    }

    void providerPicker_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<double>("zoom");
        QTest::newRow("wide-dark") << 1200 << 1.0;
        QTest::newRow("narrow-zoomed") << 440 << 1.5;
    }
    void providerPicker() {
        QFETCH(int, width);
        QFETCH(double, zoom);
        ket::Appearance::instance().setMode(width == 1200 ? ket::Appearance::Mode::Dark
                                                          : ket::Appearance::Mode::Light);
        ket::Appearance::instance().setZoom(zoom);
        AgentWorkspaceView view;
        view.applyTheme(currentThemePalette());
        view.setAgentAvailable(true);
        view.setModels({model("codex"), model("claude"), model("cursor"), model("copilot")});
        view.setConversationConfiguration(
            {{"provider", "copilot"}, {"model", "default"}, {"permission", "workspace-write"}});
        view.resize(width, 900);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        auto* root = view.quickView()->rootObject();
        QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("copilot"));
        QVERIFY(QMetaObject::invokeMethod(root, "openModelPicker"));
        auto* picker = root->property("modelPicker").value<QObject*>();
        QTRY_VERIFY(picker->property("opened").toBool());
        QCOMPARE(picker->property("selectedProviderTab").toString(), QStringLiteral("copilot"));
        auto* content = qvariant_cast<QQuickItem*>(picker->property("contentItem"));
        auto* tab = visual(content, "providerTab_copilot");
        auto* apply = visual(content, "applyModelButton");
        QVERIFY(tab && apply);
        QVERIFY(tab->isEnabled());
        QTRY_VERIFY(apply->isEnabled());
        auto* codexTab = visual(content, "providerTab_codex");
        QVERIFY(codexTab);
        QVERIFY(QMetaObject::invokeMethod(codexTab, "clicked"));
        QCOMPARE(picker->property("selectedProviderTab").toString(), QStringLiteral("codex"));
        QVERIFY(QMetaObject::invokeMethod(tab, "clicked"));
        QCOMPARE(picker->property("selectedProviderTab").toString(), QStringLiteral("copilot"));
        QTRY_VERIFY(apply->isEnabled());
        auto* tabs = visual(content, "providerTabs");
        QVERIFY(tabs);
        for (const QString id : {"codex", "claude", "cursor", "copilot", "ketrouter"}) {
            auto* item = visual(tabs, "providerTab_" + id);
            QVERIFY(item);
            QTRY_VERIFY(item->mapToItem(tabs, QPointF{}).x() + item->width() <= tabs->width() + 1);
        }
        if (width == 1200) {
            const auto capture = qEnvironmentVariable("KETPLUS_COPILOT_CAPTURE");
            if (!capture.isEmpty()) {
                QTest::qWait(100);
                QVERIFY(view.quickView()->grabWindow().save(capture));
            }
        }
        // A missing CLI opens setup, but cached models cannot be applied as runnable choices.
        view.setProviderRuntime(ProviderRuntimeStatus::stateFrom({runtime("missing")}));
        QTRY_VERIFY(!apply->isEnabled());
        auto* banner = picker->findChild<QQuickItem*>("modelPickerSetupBanner");
        QVERIFY(banner);
        QTRY_VERIFY(banner->isVisible());
        view.setModels({model("codex")});
        QTRY_VERIFY(tab->isEnabled()); // Its setup guide remains reachable.
        view.setProviderRuntime(ProviderRuntimeStatus::stateFrom({}));
        QTRY_VERIFY(!tab->isEnabled()); // An older daemon does not advertise Copilot.
        QVERIFY(!apply->isEnabled());
        QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("copilot"));
        QTRY_VERIFY(root->property("copilotUnavailable").toBool());
        auto* prompt = visual(root, "agentPrompt");
        auto* send = visual(root, "sendButton");
        QVERIFY(prompt && send);
        prompt->setProperty("text", "Keep my Copilot draft");
        QTRY_VERIFY(!send->isEnabled());
        QSignalSpy submitted(&view, &AgentWorkspaceView::configuredPromptSubmitted);
        QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
        QCOMPARE(submitted.size(), 0);
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("Keep my Copilot draft"));
    }
};

int main(int argc, char** argv) {
    QTemporaryDir settings;
    if (!settings.isValid())
        return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    app.setOrganizationName("KetPlusQA");
    app.setApplicationName("CopilotPresentation");
    CopilotPresentationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "CopilotPresentationTest.moc"
