#include "AgentWorkspaceViewTest.h"
#include "KetAppearance.h"
#include "KetProfile.h"
#include "KetRecipes.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "ui/Theme.h"

#include <QAbstractItemModelTester>
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextTable>
#include <QtTest>

#include <algorithm>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::selectsModelsInUnifiedPicker() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1000, 740);
    view.show();
    const QVariantList efforts{
        QVariantMap{{"reasoningEffort", "low"}, {"description", "Light effort"}},
        QVariantMap{{"reasoningEffort", "medium"}, {"description", "Balanced effort"}},
        QVariantMap{{"reasoningEffort", "high"}, {"description", "High effort"}}};
    const auto model = [&efforts](const QString& id, const QString& name) {
        return QVariantMap{{"provider", "codex"},
                           {"model", id},
                           {"displayName", name},
                           {"description", "Available through Codex"},
                           {"defaultReasoningEffort", "medium"},
                           {"supportedReasoningEfforts", efforts},
                           {"permissions", ketplus::CodexAppServer::permissionProfiles()},
                           {"defaultPermission", "workspace-write"}};
    };
    auto fixedEffortModel = model("fixed", "Fixed effort model");
    fixedEffortModel["defaultReasoningEffort"] = "";
    fixedEffortModel["supportedReasoningEfforts"] = QVariantList{};
    auto otherProviderModel = model("not-supported", "Unsupported provider model");
    otherProviderModel["provider"] = "claude";
    auto cursorModel = model("auto", "Cursor Auto");
    cursorModel["provider"] = "cursor";
    cursorModel["supportedReasoningEfforts"] = QVariantList{};
    cursorModel["defaultReasoningEffort"] = "";
    auto cursorLuna = model("luna", "GPT-5.6-Luna");
    cursorLuna["provider"] = "cursor";
    cursorLuna["id"] = "luna";
    const QVariantList catalog{model("astra", "GPT-6-Astra"),
                               model("luna", "GPT-5.6-Luna"),
                               fixedEffortModel,
                               otherProviderModel,
                               cursorModel,
                               cursorLuna};
    view.setModels(catalog);
    view.setConversationConfiguration(
        {{"model", "astra"}, {"effort", "medium"}, {"permission", "read-only"}});
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    QObject::connect(&view, &ketplus::AgentWorkspaceView::conversationConfigurationRequested, &view,
                     [&view](const QString& selectedModel, const QString& effort,
                             const QString& permission, const QString& provider) {
                         view.setConversationConfiguration({{"model", selectedModel},
                                                            {"effort", effort},
                                                            {"permission", permission},
                                                            {"provider", provider}});
                         view.finishModelConfiguration({});
                     });
    QSignalSpy configuration(&view,
                             &ketplus::AgentWorkspaceView::conversationConfigurationRequested);
    QSignalSpy requested(&view, &ketplus::AgentWorkspaceView::modelsRequested);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* button = root->findChild<QQuickItem*>("modelButton");
    auto* permissionButton = root->findChild<QQuickItem*>("permissionButton");
    auto* popup = root->findChild<QObject*>("modelPopup");
    auto* search = root->findChild<QQuickItem*>("modelSearch");
    auto* list = root->findChild<QQuickItem*>("modelPickerList");
    auto* slider = root->findChild<QQuickItem*>("effortSlider");
    auto* reset = root->findChild<QQuickItem*>("resetEffortButton");
    QVERIFY(button && permissionButton && popup && search && list && slider && reset);
    QCOMPARE(permissionButton->property("text").toString(), QStringLiteral("Read only"));
    root->setProperty("permission", QStringLiteral("danger-full-access"));
    QTRY_COMPARE(permissionButton->property("text").toString(), QStringLiteral("Full access"));
    root->setProperty("permission", QStringLiteral("read-only"));
    QVERIFY(!root->findChild<QObject*>("effortPopup"));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QTRY_COMPARE(list->property("count").toInt(), 3);
    QTRY_VERIFY(search->hasActiveFocus());
    QCOMPARE(requested.count(), 0);
    QCOMPARE(configuration.count(), 0);
    auto* popupContent = qvariant_cast<QQuickItem*>(popup->property("contentItem"));
    QVERIFY(popupContent);
    for (const auto& id : {"claude", "codex", "cursor", "ketrouter"}) {
        auto* tab =
            findVisualItem(popupContent, QStringLiteral("providerTab_") + QString::fromLatin1(id));
        QVERIFY(tab);
        const bool expectedEnabled = QString::fromLatin1(id) == QStringLiteral("codex") ||
                                     QString::fromLatin1(id) == QStringLiteral("claude") ||
                                     QString::fromLatin1(id) == QStringLiteral("cursor");
        QCOMPARE(tab->isEnabled(), expectedEnabled);
        QVERIFY(tab->width() < 90.0);
    }
    auto* claudeTab = findVisualItem(popupContent, QStringLiteral("providerTab_claude"));
    auto* codexTab = findVisualItem(popupContent, QStringLiteral("providerTab_codex"));
    auto* cursorTab = findVisualItem(popupContent, QStringLiteral("providerTab_cursor"));
    QVERIFY(claudeTab && codexTab && cursorTab);
    QVERIFY(QMetaObject::invokeMethod(claudeTab, "clicked"));
    QTRY_COMPARE(list->property("count").toInt(), 1);
    QVERIFY(QMetaObject::invokeMethod(cursorTab, "clicked"));
    QTRY_COMPARE(list->property("count").toInt(), 2);
    QTRY_VERIFY(!slider->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(codexTab, "clicked"));
    QTRY_COMPARE(list->property("count").toInt(), 3);
    // Future turn defaults stay editable while the current provider keeps running.
    view.setConversationConfiguration(QVariantMap{{"model", "astra"},
                                                  {"effort", "medium"},
                                                  {"permission", "read-only"},
                                                  {"provider", "codex"}});
    QTRY_VERIFY(claudeTab->isEnabled());
    view.setSessionState(QStringLiteral("Conversation"), QStringLiteral("Working…"), true);
    QTRY_VERIFY(claudeTab->isEnabled());
    QVERIFY(codexTab->isEnabled());
    view.setSessionState(QStringLiteral("Conversation"), QStringLiteral("Ready"), false);
    QTRY_VERIFY(claudeTab->isEnabled());
    const auto capture = [&](const QString& suffix) {
        const QString prefix = qEnvironmentVariable("KETPLUS_MODEL_PICKER_CAPTURE");
        if (prefix.isEmpty())
            return true;
        const auto shot = view.quickView()->grabWindow();
        return !shot.isNull() && shot.save(prefix + suffix + ".png");
    };
    QTest::qWait(100);
    QVERIFY(capture(".dark"));
    QCOMPARE(qRound(popup->property("x").toDouble() * 2.0 + popup->property("width").toDouble()),
             qRound(root->width()));
    QCOMPARE(qRound(popup->property("y").toDouble() * 2.0 + popup->property("height").toDouble()),
             qRound(root->height()));
    search->setProperty("text", "  LUNA  ");
    QTRY_COMPARE(list->property("count").toInt(), 1);
    QTest::keyClick(view.quickView(), Qt::Key_Down);
    QTRY_VERIFY(list->hasActiveFocus());
    QCOMPARE(list->property("currentIndex").toInt(), 0);
    QTest::keyClick(view.quickView(), Qt::Key_Return);
    QTRY_COMPARE(popup->property("draftModel").toString(), QStringLiteral("luna"));
    QCOMPARE(view.conversationModel(), QStringLiteral("astra"));
    QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("codex"));
    QCOMPARE(root->property("permission").toString(), QStringLiteral("read-only"));
    QVERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("selectedProviderTab").toString(), QStringLiteral("codex"));
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("selectedProviderTab").toString(), QStringLiteral("codex"));
    QCOMPARE(list->property("count").toInt(), 3);
    slider->setProperty("value", 2);
    QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
    QCOMPARE(popup->property("draftEffort").toString(), QStringLiteral("high"));
    QVERIFY(reset->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(reset, "clicked"));
    QCOMPARE(popup->property("draftEffort").toString(), QStringLiteral("medium"));
    QVERIFY(!reset->isEnabled());
    search->setProperty("text", "no matching model");
    QTRY_COMPARE(list->property("count").toInt(), 0);
    auto* empty = root->findChild<QQuickItem*>("modelPickerEmptyState");
    auto* retry = root->findChild<QQuickItem*>("retryModelsButton");
    QVERIFY(empty && retry);
    QCOMPARE(empty->property("text").toString(), QStringLiteral("No models found"));
    QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
    QTRY_COMPARE(list->property("count").toInt(), 3);
    QCOMPARE(requested.count(), 0);
    search->setProperty("text", "fixed");
    QVERIFY(QMetaObject::invokeMethod(popup, "chooseCurrentModel"));
    QCOMPARE(popup->property("draftModel").toString(), QStringLiteral("fixed"));
    QVERIFY(!slider->isEnabled());
    QVERIFY(reset->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(reset, "clicked"));
    QCOMPARE(view.conversationModel(), QStringLiteral("astra"));
    QVERIFY(!reset->isEnabled());
    view.setConversationConfiguration(
        {{"model", "astra"}, {"effort", "high"}, {"permission", "read-only"}});
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_COMPARE(slider->property("value").toInt(), 2);
    QVERIFY(slider->isEnabled());
    slider->setProperty("value", 1);
    QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
    auto* effortHandle = findVisualItem(popupContent, "effortHandle");
    auto* selectedEffortTick = findVisualItem(popupContent, "effortTickLabel_1");
    QVERIFY(effortHandle && selectedEffortTick);
    const auto handleCenter =
        effortHandle->mapToItem(root, QPointF(effortHandle->width() / 2.0, 0.0)).x();
    const auto labelCenter =
        selectedEffortTick->mapToItem(root, QPointF(selectedEffortTick->width() / 2.0, 0.0)).x();
    QVERIFY2(qAbs(labelCenter - handleCenter) <= 1.0,
             qPrintable(QStringLiteral("Effort label center %1 must match handle center %2")
                            .arg(labelCenter)
                            .arg(handleCenter)));
    slider->setProperty("value", 2);
    QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
    search->setProperty("text", "");
    const auto assertBounds = [&]() {
        const QRectF rect(popup->property("x").toDouble(), popup->property("y").toDouble(),
                          popup->property("width").toDouble(),
                          popup->property("height").toDouble());
        return rect.left() >= 0 && rect.top() >= 0 && rect.right() <= root->width() &&
               rect.bottom() <= root->height() && list->height() > 0;
    };
    QVERIFY(assertBounds());
    view.resize(420, 480);
    QTest::qWait(100);
    QVERIFY(assertBounds());
    auto* tabs = root->findChild<QQuickItem*>("providerTabs");
    QVERIFY(tabs);
    QVERIFY(tabs->implicitWidth() < popup->property("width").toDouble());
    QVERIFY(capture(".narrow"));
    view.resize(1000, 740);
    ketplus::ThemePalette light = testPalette();
    light.dark = false;
    light.pageBackground = "#ffffff";
    light.sidebarBackground = "#f4f5f7";
    light.surfaceRaised = "#ffffff";
    light.surfaceHover = "#edf0f4";
    light.textMain = "#20242b";
    light.textSecondary = "#424955";
    light.textMuted = "#657080";
    light.interactiveActive = "#e8ebf0";
    light.interactiveBackground = "#f5f6f8";
    light.accentSubtle = "#edf0ff";
    light.accentMuted = "#c5ceff";
    light.border = "#e0e3e9";
    light.borderStrong = "#bcc3cf";
    view.applyTheme(light);
    QTest::qWait(100);
    QCOMPARE(view.conversationModel(), QStringLiteral("astra"));
    QCOMPARE(view.conversationEffort(), QStringLiteral("high"));
    QVERIFY(capture(".light"));
    QTest::keyClick(view.quickView(), Qt::Key_Escape);
    QTRY_VERIFY(!popup->property("visible").toBool());
    QTRY_VERIFY(button->hasActiveFocus());
    view.setModels({});
    const int configCount = configuration.count();
    const QVariantMap command{{"id", "model"}};
    QVERIFY(QMetaObject::invokeMethod(root, "applySlashCommand", Q_ARG(QVariant, command)));
    QTRY_VERIFY(popup->property("visible").toBool());
    QTRY_VERIFY(search->hasActiveFocus());
    QCOMPARE(requested.count(), 1);
    QVERIFY(root->property("modelsLoading").toBool());
    QVERIFY(!slider->isEnabled());
    view.setModels({});
    QVERIFY(!root->property("modelsLoading").toBool());
    QCOMPARE(empty->property("text").toString(), QStringLiteral("Models unavailable"));
    QVERIFY(QMetaObject::invokeMethod(retry, "clicked"));
    QCOMPARE(requested.count(), 2);
    view.setModels(catalog);
    QTRY_COMPARE(root->property("selectedModel").toString(), QStringLiteral("astra"));
    QCOMPARE(configuration.count(), configCount);
    view.setAgentAvailable(false);
    QVERIFY(!slider->isEnabled());
    QVERIFY(!list->isVisible());
    QCOMPARE(empty->property("text").toString(), QStringLiteral("Codex CLI unavailable"));
}

void AgentWorkspaceViewTest::restoresStoredConversationModelInsteadOfCatalogDefault() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1000, 740);
    view.show();
    const QVariantList efforts{
        QVariantMap{{"reasoningEffort", "low"}, {"description", "Light effort"}},
        QVariantMap{{"reasoningEffort", "medium"}, {"description", "Balanced effort"}},
        QVariantMap{{"reasoningEffort", "high"}, {"description", "High effort"}}};
    const auto model = [&efforts](const QString& id, const QString& name, const QString& provider) {
        return QVariantMap{{"provider", provider},
                           {"model", id},
                           {"id", id},
                           {"displayName", name},
                           {"description", "Available through " + provider},
                           {"defaultReasoningEffort", provider == QStringLiteral("codex")
                                                          ? QStringLiteral("medium")
                                                          : QString{}},
                           {"supportedReasoningEfforts",
                            provider == QStringLiteral("codex") ? efforts : QVariantList{}},
                           {"permissions", ketplus::CodexAppServer::permissionProfiles()},
                           {"defaultPermission", "workspace-write"}};
    };
    auto cursorAuto = model("auto", "Cursor Auto", "cursor");
    cursorAuto["isDefault"] = true;
    const QVariantList catalog{model("astra", "GPT-6-Astra", "codex"),
                               model("luna", "GPT-5.6-Luna", "codex"),
                               model("luna", "GPT-5.6-Luna", "cursor"), cursorAuto};
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    view.setConversationConfiguration({{"model", "astra"},
                                       {"effort", "high"},
                                       {"permission", "read-only"},
                                       {"provider", "codex"}});
    view.setModels(catalog);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QTRY_COMPARE(root->property("selectedModel").toString(), QStringLiteral("astra"));
    QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("codex"));
    QCOMPARE(view.conversationModel(), QStringLiteral("astra"));
    auto composerLabel = [&] {
        QVariant label;
        if (!QMetaObject::invokeMethod(root, "composerModelText", Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariant, label)))
            return QString{};
        return label.toString();
    };
    QString label = composerLabel();
    QVERIFY2(label.startsWith(QStringLiteral("Codex · ")), qPrintable(label));
    QVERIFY2(label.contains(QStringLiteral("Astra")), qPrintable(label));
    QVERIFY(!label.contains(QStringLiteral("Auto")));

    view.setConversationConfiguration({{"model", "auto"},
                                       {"effort", ""},
                                       {"permission", "workspace-write"},
                                       {"provider", "cursor"}});
    QTRY_COMPARE(root->property("selectedModel").toString(), QStringLiteral("auto"));
    QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("cursor"));
    label = composerLabel();
    QVERIFY2(label.startsWith(QStringLiteral("Cursor · ")), qPrintable(label));
    QVERIFY2(label.contains(QStringLiteral("Auto")), qPrintable(label));

    view.setConversationConfiguration({{"model", "astra"},
                                       {"effort", "high"},
                                       {"permission", "read-only"},
                                       {"provider", "codex"}});
    QTRY_COMPARE(root->property("selectedModel").toString(), QStringLiteral("astra"));
    QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("codex"));
    label = composerLabel();
    QVERIFY2(label.startsWith(QStringLiteral("Codex · ")), qPrintable(label));
    QVERIFY2(label.contains(QStringLiteral("Astra")), qPrintable(label));
    QVERIFY(!label.contains(QStringLiteral("Auto")));

    view.setConversationConfiguration({{"model", "luna"},
                                       {"effort", "medium"},
                                       {"permission", "workspace-write"},
                                       {"provider", "codex"}});
    QTRY_COMPARE(root->property("selectedModel").toString(), QStringLiteral("luna"));
    QCOMPARE(root->property("selectedProvider").toString(), QStringLiteral("codex"));
}

void AgentWorkspaceViewTest::prefetchesModelsWhenAgentBecomesAvailable() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(false);
    view.resize(900, 680);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    QSignalSpy requested(&view, &ketplus::AgentWorkspaceView::modelsRequested);
    view.setAgentAvailable(true);
    QTRY_COMPARE(requested.count(), 1);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* button = root->findChild<QQuickItem*>("modelButton");
    QVERIFY(button);
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(root->findChild<QObject*>("modelPopup")->property("visible").toBool());
    QCOMPARE(requested.count(), 1);
}

void AgentWorkspaceViewTest::worktreeHeaderKeepsLabelVisible() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setSessionState("CRM Refactor", "Working", true);
    view.setExecutionBranch("feat/customer-care-routine-workflow");
    view.resize(1200, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* label = root->findChild<QQuickItem*>("workspaceScopeLabel");
    auto* chip = root->findChild<QQuickItem*>("workspaceScopeChip");
    QVERIFY(label && chip);
    for (const int width : {1200, 900, 1600}) {
        view.resize(width, 700);
        QTRY_VERIFY(label->width() > 40);
        QVERIFY(label->property("text").toString().contains("feat/customer-care-routine-workflow"));
        QVERIFY(label->width() <= label->implicitWidth());
        QVERIFY(label->x() + label->width() <= chip->width());
    }
    view.setExecutionBranch("fix/short");
    QTRY_VERIFY(label->property("text").toString().endsWith("fix/short"));
    QTRY_VERIFY(label->width() > 40);
    const auto capture = qEnvironmentVariable("KETPLUS_HEADER_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}

void AgentWorkspaceViewTest::composerPreview() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(1000, 700);
    view.show();
    QTest::qWait(100);
    auto* root = view.quickView()->rootObject();
    auto* draft = root->findChild<QQuickItem*>(QStringLiteral("agentPrompt"));
    QVERIFY(draft);
    view.setSessionState(QStringLiteral("Chat"), QStringLiteral("Working…"), true);
    view.setAgentAvailable(false);
    view.setSessionState(QStringLiteral("Chat"), QStringLiteral("Ready"), false);
    QVERIFY(draft->isEnabled());
    draft->setProperty("text", QStringLiteral("Next request"));
    QCOMPARE(draft->property("text").toString(), QStringLiteral("Next request"));
    draft->setProperty("text", QString());
    view.setAgentAvailable(true);
    QVERIFY(root);
    auto* composer = root->findChild<QQuickItem*>("agentComposer");
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    auto* sidebar = root->findChild<QQuickItem*>("worktreeSidebar");
    auto* productNavigationLayout = root->findChild<QQuickItem*>("agentProductNavigation");
    auto* brand = root->findChild<QQuickItem*>("productBrand");
    auto* quickSearch = root->findChild<QQuickItem*>("productQuickSearch");
    auto* productNavigationGroup = root->findChild<QQuickItem*>("productNavigation");
    auto* productNavigationRepeater = root->findChild<QObject*>("productNavigationRepeater");
    auto* composerFooter = root->findChild<QQuickItem*>("composerFooter");
    auto* identity = root->findChild<QQuickItem*>("productIdentity");
    auto* workspaceHeader = root->findChild<QQuickItem*>("agentWorkspaceHeader");
    auto* conversationTitle = root->findChild<QQuickItem*>("conversationTitle");
    auto* conversationStatus = root->findChild<QQuickItem*>("conversationStatusBadge");
    QVERIFY(composer);
    QVERIFY(prompt);
    QVERIFY(sidebar);
    QVERIFY(productNavigationLayout);
    QVERIFY(brand);
    QVERIFY(quickSearch);
    QVERIFY(productNavigationGroup);
    QVERIFY(productNavigationRepeater);
    QCOMPARE(productNavigationRepeater->property("count").toInt(), 4);
    QVERIFY(composerFooter);
    QVERIFY(identity);
    QVERIFY(workspaceHeader);
    QVERIFY(conversationTitle);
    QVERIFY(conversationStatus);
    QVERIFY(sidebar->width() >= 188.0 && sidebar->width() <= 320.0);
    QCOMPARE(productNavigationLayout->height(), productNavigationLayout->implicitHeight());
    QVERIFY(productNavigationLayout->mapToItem(sidebar, QPointF()).y() >= 0.0);
    QCOMPARE(brand->height(), 54.0);
    QCOMPARE(quickSearch->height(), quickSearch->implicitHeight());
    QVERIFY(composerFooter->isVisible());
    QTRY_VERIFY(identity->mapToItem(sidebar, QPointF(0, identity->height())).y() <=
                sidebar->height() + 1.0);
    const auto titleRight =
        conversationTitle->mapToItem(workspaceHeader, QPointF(conversationTitle->width(), 0));
    const auto statusLeft = conversationStatus->mapToItem(workspaceHeader, QPointF(0, 0));
    QVERIFY(qAbs(statusLeft.x() - titleRight.x() - 8.0) < 1.0);
    QSignalSpy productNavigation(&view, &ketplus::AgentWorkspaceView::productNavigationRequested);
    QVERIFY(QMetaObject::invokeMethod(quickSearch, "clicked"));
    QCOMPARE(productNavigation.count(), 1);
    QCOMPARE(productNavigation.constFirst().constFirst().toString(), QStringLiteral("search"));
    view.resize(1200, 900);
    QTRY_COMPARE(brand->height(), 67.0);
    QTRY_COMPARE(composer->width(), 720.0);
    QTRY_VERIFY(identity->mapToItem(sidebar, QPointF(0, identity->height())).y() <=
                sidebar->height() + 1.0);
    auto* suggestion = findVisualItem(root, QStringLiteral("workspaceSuggestion"));
    QVERIFY(suggestion);
    QVERIFY(suggestion->width() >= 44 && suggestion->height() >= 44);
    QVERIFY(QMetaObject::invokeMethod(suggestion, "clicked"));
    QVERIFY(!prompt->property("text").toString().isEmpty());
    prompt->setProperty("text", QString{});

    QTemporaryDir skillWorkspace;
    QVERIFY(skillWorkspace.isValid());
    const QString skillDirectory =
        skillWorkspace.filePath(QStringLiteral(".agents/skills/reviewer"));
    QVERIFY(QDir().mkpath(skillDirectory));
    QFile skillFile(QDir(skillDirectory).filePath(QStringLiteral("SKILL.md")));
    QVERIFY(skillFile.open(QIODevice::WriteOnly | QIODevice::Text));
    skillFile.write("---\nname: reviewer\ndescription: Review the current change.\n---\n");
    skillFile.close();
    QSignalSpy scannedSkills(&view, &ketplus::AgentWorkspaceView::skillsChanged);
    view.setActivePath(skillWorkspace.path());
    QTRY_VERIFY(!scannedSkills.isEmpty());
    prompt->setProperty("text", QStringLiteral("$rev"));
    prompt->setProperty("cursorPosition", 4);
    QTest::qWait(100);
    auto* skillMentionPalette = root->findChild<QQuickItem*>(QStringLiteral("skillMentionPalette"));
    QVERIFY(skillMentionPalette);
    QVERIFY(skillMentionPalette->isVisible());
    auto* paletteComposer = root->findChild<QQuickItem*>(QStringLiteral("agentComposer"));
    QVERIFY(paletteComposer);
    // The popover floats above the composer instead of growing it and pushing the transcript.
    const QPointF paletteBottom =
        skillMentionPalette->mapToItem(paletteComposer, QPointF(0, skillMentionPalette->height()));
    QVERIFY(paletteBottom.y() <= 0.0);
    const QVariantList skillSuggestions = root->property("visibleSkillSuggestions").toList();
    QVariant reviewerSuggestion;
    for (const QVariant& value : skillSuggestions) {
        if (value.toMap().value(QStringLiteral("mention")).toString() ==
            QStringLiteral("$reviewer")) {
            reviewerSuggestion = value;
            break;
        }
    }
    QVERIFY(reviewerSuggestion.isValid());
    QVERIFY(QMetaObject::invokeMethod(root, "applySkillSuggestion",
                                      Q_ARG(QVariant, reviewerSuggestion)));
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("$reviewer "));
    view.setActivePath(QString());

    prompt->setProperty("text",
                        "Update the chat UI and add model, reasoning and attachment controls.");
    QTest::qWait(100);
    QVERIFY(composer->height() >= 40);
    view.setSessionState(QStringLiteral("New conversation"), QStringLiteral("Thinking..."), true);
    QTest::qWait(100);
    auto* workingIndicator = root->findChild<QQuickItem*>(QStringLiteral("workingIndicator"));
    QVERIFY(workingIndicator && workingIndicator->isVisible());
    view.setSessionState(QStringLiteral("New conversation"), QStringLiteral("Ready"), false);
    QTest::qWait(100);
    view.resetMessages(
        {QVariantMap{{QStringLiteral("kind"), QStringLiteral("user")},
                     {QStringLiteral("body"), QStringLiteral("Visible user message")},
                     {QStringLiteral("meta"), QStringLiteral("You")},
                     {QStringLiteral("filePath"), QString()}}});
    QTest::qWait(100);
    QVariant userSurfaceWidth;
    QVERIFY(QMetaObject::invokeMethod(root, "debugMessageSurfaceWidth",
                                      Q_RETURN_ARG(QVariant, userSurfaceWidth),
                                      Q_ARG(QVariant, 0)));
    QVERIFY2(
        userSurfaceWidth.toDouble() > 120.0,
        qPrintable(QStringLiteral("User messages must render with a readable bubble width; got %1.")
                       .arg(userSurfaceWidth.toDouble())));
    QTemporaryDir attachmentDirectory;
    const QString previewPath = attachmentDirectory.filePath("preview.png");
    QImage previewImage(32, 32, QImage::Format_RGB32);
    previewImage.fill(Qt::cyan);
    QVERIFY(previewImage.save(previewPath));
    view.addAttachments({previewPath});
    QCOMPARE(view.attachments().size(), 1);
    view.addAttachments({previewPath});
    QCOMPARE(view.attachments().size(), 1);
    QQuickItem* composerImage = nullptr;
    QTRY_VERIFY((composerImage = findVisualItem(root, QStringLiteral("composerAttachmentImage"))));
    QTRY_COMPARE(composerImage->property("status").toInt(), 1); // Image.Ready
    auto* chip = findVisualItem(root, QStringLiteral("composerAttachmentChip"));
    QVERIFY(chip);
    QVERIFY(QMetaObject::invokeMethod(chip, "previewAttachment"));
    auto* imagePreview = root->findChild<QObject*>(QStringLiteral("imagePreview"));
    QVERIFY(imagePreview);
    QTRY_VERIFY(imagePreview->property("visible").toBool());
    QCOMPARE(imagePreview->property("imageSource").toUrl().toLocalFile(), previewPath);
    QVERIFY(QMetaObject::invokeMethod(imagePreview, "close"));
    QTRY_VERIFY(!imagePreview->property("visible").toBool());
    const QString capture = qEnvironmentVariable("KETPLUS_COMPOSER_CAPTURE");
    if (!capture.isEmpty()) {
        const auto shot = view.quickView()->grabWindow();
        QVERIFY(!shot.isNull());
        QVERIFY(shot.save(capture));
    }
    auto* permissionButton = root->findChild<QQuickItem*>("permissionButton");
    QVERIFY(permissionButton);
    QVERIFY(QMetaObject::invokeMethod(permissionButton, "clicked"));
    QTest::qWait(100);
    if (!capture.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capture + ".permissions.png"));
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject*>("permissionMenu"), "close"));
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject*>("modelPopup"), "close"));
    view.setModels({QVariantMap{
        {"provider", "codex"},
        {"model", "test-model"},
        {"displayName", "Test model"},
        {"description", "Test catalog"},
        {"defaultReasoningEffort", "medium"},
        {"supportedReasoningEfforts",
         QVariantList{QVariantMap{{"reasoningEffort", "low"},
                                  {"description", "Quick responses for simple tasks"}},
                      QVariantMap{{"reasoningEffort", "medium"},
                                  {"description", "Balanced reasoning and speed"}},
                      QVariantMap{{"reasoningEffort", "high"},
                                  {"description", "More reasoning for difficult tasks"}}}},
        {"permissions", ketplus::CodexAppServer::permissionProfiles()},
        {"defaultPermission", "workspace-write"}}});
    auto* modelButton = root->findChild<QQuickItem*>("modelButton");
    QVERIFY(modelButton);
    QVERIFY(QMetaObject::invokeMethod(root, "selectModel", Q_ARG(QVariant, view.models().first()),
                                      Q_ARG(QVariant, false)));
    QCOMPARE(root->property("selectedModel").toString(), "test-model");
    QVERIFY(QMetaObject::invokeMethod(modelButton, "clicked"));
    QTest::qWait(100);
    if (!capture.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capture + ".effort.png"));
    auto* slider = root->findChild<QObject*>("effortSlider");
    QVERIFY(slider);
    slider->setProperty("value", 2);
    QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
    QCOMPARE(root->findChild<QObject*>("modelPopup")->property("draftEffort").toString(), "high");
    QCOMPARE(root->property("selectedEffort").toString(), "medium");
    QVERIFY(root->findChild<QObject*>("modelPopup")->property("visible").toBool());
    QVERIFY(root->findChild<QObject*>("modelSearch"));
    if (!capture.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capture + ".models.png"));
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject*>("permissionMenu"), "close"));
    QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject*>("modelPopup"), "close"));
    prompt->forceActiveFocus();
    prompt->setProperty("text", "/term");
    QTest::qWait(100);
    auto* slashPalette = root->findChild<QQuickItem*>("slashPalette");
    QVERIFY(slashPalette);
    QVERIFY(slashPalette->isVisible());
    QSignalSpy terminalFromSlash(&view, &ketplus::AgentWorkspaceView::terminalRequested);
    QVariantMap terminalCommand{{QStringLiteral("id"), QStringLiteral("terminal")}};
    QVERIFY(QMetaObject::invokeMethod(root, "applySlashCommand", Q_ARG(QVariant, terminalCommand)));
    QCOMPARE(terminalFromSlash.count(), 1);
    QCOMPARE(prompt->property("text").toString(), QString());
    QVariantMap planCommand{{QStringLiteral("id"), QStringLiteral("plan")}};
    QVERIFY(QMetaObject::invokeMethod(root, "applySlashCommand", Q_ARG(QVariant, planCommand)));
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("/plan "));
    QSignalSpy planSubmitted(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    prompt->setProperty("text", QStringLiteral("/plan Add command-only planning"));
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QCOMPARE(planSubmitted.count(), 1);
    QCOMPARE(planSubmitted.constFirst().at(0).toString(),
             QStringLiteral("Add command-only planning"));
    QCOMPARE(planSubmitted.constFirst().at(1).toMap().value(QStringLiteral("mode")).toString(),
             QStringLiteral("plan"));
    view.resize(640, 600);
    QTest::qWait(100);
    QVERIFY(composer->width() > 200);
    if (!capture.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capture + ".narrow.png"));
    prompt->setProperty("text", QStringLiteral("Draft survives worktree switch."));
    view.setActivePath(attachmentDirectory.path());
    QCOMPARE(view.attachments().size(), 0);
    QCOMPARE(prompt->property("text").toString(), QString());
    view.setActivePath(QString());
    QCOMPARE(view.attachments().size(), 1);
    QVERIFY(!prompt->property("text").toString().isEmpty());
    view.removeAttachment(0);
    QCOMPARE(view.attachments().size(), 0);
    root->setProperty("permission", "danger-full-access");
    QVariantMap compatible = view.models().first().toMap();
    compatible.insert("provider", "another-compatible-provider");
    QVERIFY(QMetaObject::invokeMethod(root, "selectModel", Q_ARG(QVariant, QVariant(compatible)),
                                      Q_ARG(QVariant, false)));
    QCOMPARE(root->property("permission").toString(), "danger-full-access");
    const QVariantMap otherProvider{
        {"provider", "future-provider"},
        {"model", "future-model"},
        {"displayName", "Future model"},
        {"permissions",
         QVariantList{QVariantMap{{"id", "provider-default"}, {"label", "Default"}}}},
        {"defaultPermission", "provider-default"},
        {"supportedReasoningEfforts", QVariantList{}},
        {"defaultReasoningEffort", ""}};
    QVERIFY(QMetaObject::invokeMethod(root, "selectModel", Q_ARG(QVariant, QVariant(otherProvider)),
                                      Q_ARG(QVariant, false)));
    QCOMPARE(root->property("permission").toString(), "provider-default");
    QCOMPARE(root->property("selectedProvider").toString(), "future-provider");
}

void AgentWorkspaceViewTest::tooltipsFollowDesignSystem() {
    ketplus::AgentWorkspaceView view;
    const auto palette = testPalette();
    view.applyTheme(palette);
    view.setAgentAvailable(true);
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* attachButton = root->findChild<QQuickItem*>(QStringLiteral("attachButton"));
    QVERIFY(attachButton);
    auto* tooltip = attachButton->findChild<QObject*>(QStringLiteral("ketToolTip"));
    QVERIFY(tooltip);

    const auto recipe = ket::tooltipRecipe(ket::Appearance::instance().theme());
    const auto zoom = ket::Appearance::instance().zoom();
    QCOMPARE(tooltip->property("backgroundColor").value<QColor>(), recipe.background);
    QCOMPARE(tooltip->property("labelColor").value<QColor>(), recipe.text);
    QCOMPARE(tooltip->property("cornerRadius").toDouble(),
             ket::Appearance::scale(recipe.radius, zoom));

    auto* background = qvariant_cast<QObject*>(tooltip->property("background"));
    auto* contentItem = qvariant_cast<QObject*>(tooltip->property("contentItem"));
    QVERIFY(background);
    QVERIFY(contentItem);
    QCOMPARE(background->property("color").value<QColor>(), recipe.background);
    QCOMPARE(background->property("radius").toDouble(),
             ket::Appearance::scale(recipe.radius, zoom));
    QCOMPARE(contentItem->property("color").value<QColor>(), recipe.text);

    tooltip->setProperty("visible", true);
    QTRY_VERIFY(tooltip->property("visible").toBool());
    const QString capture = qEnvironmentVariable("KETPLUS_TOOLTIP_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(100);
        const auto shot = view.quickView()->grabWindow();
        QVERIFY(!shot.isNull());
        QVERIFY(shot.save(capture));
    }
}

void AgentWorkspaceViewTest::contextMenuFollowsDesignSystem() {
    ketplus::AgentWorkspaceView view;
    const auto palette = testPalette();
    view.applyTheme(palette);
    view.setConversations({QVariantMap{{"id", "conversation"},
                                       {"title", "Conversation"},
                                       {"provider", "codex"},
                                       {"active", true},
                                       {"running", false},
                                       {"status", "Ready"}}});
    view.resize(1000, 700);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QQuickItem* conversationRow = nullptr;
    QTRY_VERIFY(
        (conversationRow = findVisualItem(root, QStringLiteral("conversationRow_conversation"))));
    auto* menu = qvariant_cast<QObject*>(conversationRow->property("contextMenu"));
    QVERIFY(menu);
    QCOMPARE(menu->property("backgroundColor").value<QColor>(), QColor(palette.panelBackground));
    QCOMPARE(menu->property("borderColor").value<QColor>(), QColor(palette.borderStrong));
    QCOMPARE(menu->property("cornerRadius").toDouble(), 7.0);
    QCOMPARE(menu->property("padding").toDouble(), 5.0);
    QCOMPARE(menu->property("opacity").toDouble(), 1.0);

    auto* menuBackground = qvariant_cast<QObject*>(menu->property("background"));
    auto* renameItem = qvariant_cast<QObject*>(conversationRow->property("renameMenuItem"));
    QVERIFY(menuBackground);
    QVERIFY(renameItem);
    QCOMPARE(menuBackground->property("color").value<QColor>(), QColor(palette.panelBackground));
    QCOMPARE(menuBackground->property("radius").toDouble(), 7.0);
    QCOMPARE(renameItem->property("implicitHeight").toDouble(), 34.0);

    auto* itemContent = qvariant_cast<QObject*>(renameItem->property("contentItem"));
    auto* itemBackground = qvariant_cast<QObject*>(renameItem->property("background"));
    QVERIFY(itemContent);
    QVERIFY(itemBackground);
    QCOMPARE(itemContent->property("color").value<QColor>(), QColor(palette.textMain));
    QCOMPARE(itemBackground->property("radius").toDouble(), 5.0);

    QVERIFY(QMetaObject::invokeMethod(menu, "open"));
    QTRY_VERIFY(menu->property("visible").toBool());
    menu->setProperty("currentIndex", 0);
    QTRY_VERIFY(renameItem->property("highlighted").toBool());
    QTRY_COMPARE(itemContent->property("color").value<QColor>(), QColor(palette.accentText));
    QTRY_COMPARE(itemBackground->property("color").value<QColor>(), QColor(palette.accentSubtle));
    const QString capture = qEnvironmentVariable("KETPLUS_CONTEXT_MENU_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(100);
        const auto shot = view.quickView()->grabWindow();
        QVERIFY(!shot.isNull());
        QVERIFY(shot.save(capture));
    }
}

void AgentWorkspaceViewTest::userMessageBubbleIgnoresUnavailableImage() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();

    view.resetMessages(
        {QVariantMap{{QStringLiteral("kind"), QStringLiteral("user")},
                     {QStringLiteral("body"),
                      QStringLiteral("Compare memory use across several repositories, worktrees, "
                                     "and active conversations while keeping this message long "
                                     "enough to wrap onto multiple lines.")},
                     {QStringLiteral("images"), QVariantList{QString{}}},
                     {QStringLiteral("meta"), QStringLiteral("You")}}});

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QTRY_COMPARE(transcript->property("count").toInt(), 1);
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* surface = findVisualItem(root, QStringLiteral("messageSurface"));
    auto* body = findVisualItem(root, QStringLiteral("messageBody"));
    QVERIFY(surface && body);
    QTRY_VERIFY(body->property("contentHeight").toDouble() > 0);

    const double expectedHeight = body->property("contentHeight").toDouble() + 16.0;
    QTRY_VERIFY2(qAbs(surface->height() - expectedHeight) < 1.0,
                 qPrintable(QStringLiteral("User bubble height %1 must fit text content height %2 "
                                           "plus 16 px vertical padding when attachment previews "
                                           "are unavailable.")
                                .arg(surface->height())
                                .arg(body->property("contentHeight").toDouble())));
}

void AgentWorkspaceViewTest::copiesConversationTranscript() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    QTest::qWait(100);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    view.resetMessages(
        {QVariantMap{{QStringLiteral("kind"), QStringLiteral("user")},
                     {QStringLiteral("body"), QStringLiteral("Add copy support")},
                     {QStringLiteral("meta"), QStringLiteral("You")},
                     {QStringLiteral("filePath"), QString()}},
         QVariantMap{{QStringLiteral("kind"), QStringLiteral("assistant")},
                     {QStringLiteral("body"), QStringLiteral("Done")},
                     {QStringLiteral("meta"), QStringLiteral("KetPlus Agent")},
                     {QStringLiteral("filePath"), QString()}},
         QVariantMap{
             {QStringLiteral("kind"), QStringLiteral("file")},
             {QStringLiteral("body"),
              QStringLiteral("src/agent/AgentWorkspaceView.cpp\nsrc/agent/qml/AgentWorkspace.qml")},
             {QStringLiteral("meta"), QStringLiteral("Changed files")},
             {QStringLiteral("filePath"), QString()}}});
    QTest::qWait(100);
    // The footer button and the transcript's context menu both copy the conversation; while a
    // turn runs they copy what is loaded.
    auto* copyButton = root->findChild<QQuickItem*>(QStringLiteral("copyConversationButton"));
    QVERIFY(copyButton);
    QVERIFY(copyButton->isVisible());
    auto* menu = root->findChild<QObject*>(QStringLiteral("transcriptContextMenu"));
    QVERIFY(menu);
    auto* copyItem =
        menu->findChild<QObject*>(QStringLiteral("copyTranscriptConversationMenuItem"));
    QVERIFY(copyItem);
    QCOMPARE(copyItem->property("text").toString(), QStringLiteral("Copy conversation"));
    view.setSessionState(QStringLiteral("Copy test"), QStringLiteral("Working..."), true);
    QTRY_COMPARE(copyItem->property("text").toString(), QStringLiteral("Copy loaded conversation"));
    QVERIFY(copyButton->isVisible());
    view.setSessionState(QStringLiteral("Copy test"), QStringLiteral("Ready"), false);
    QTRY_COMPARE(copyItem->property("text").toString(), QStringLiteral("Copy conversation"));
    QVariant selectable;
    QVERIFY(QMetaObject::invokeMethod(root, "debugMessageBodySelectable",
                                      Q_RETURN_ARG(QVariant, selectable), Q_ARG(QVariant, 0)));
    QVERIFY(selectable.toBool());
    QVariant transcript;
    QVERIFY(QMetaObject::invokeMethod(root, "conversationCopyText",
                                      Q_RETURN_ARG(QVariant, transcript)));
    view.copyText(transcript.toString());
    QCOMPARE(QGuiApplication::clipboard()->text(),
             QStringLiteral(
                 "You:\nAdd copy support\n\nKetPlus "
                 "Agent:\nDone\n\nChanged files:\nsrc/agent/AgentWorkspaceView.cpp\nsrc/agent/"
                 "qml/AgentWorkspace.qml"));
}

void AgentWorkspaceViewTest::runningIndicatorDoesNotOverlapLastCommand() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));

    QVariantList commands;
    for (int index = 0; index < 3; ++index) {
        const QString command = QStringLiteral("/bin/zsh -lc 'command-%1'").arg(index + 1);
        commands.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("tool")},
                                    {QStringLiteral("subtype"), QStringLiteral("command")},
                                    {QStringLiteral("body"), command},
                                    {QStringLiteral("command"), command},
                                    {QStringLiteral("meta"), QStringLiteral("Ran")},
                                    {QStringLiteral("running"), false},
                                    {QStringLiteral("filePath"), QString()}});
    }
    view.resetMessages(commands);
    view.setSessionState(QStringLiteral("Command test"), QStringLiteral("Working..."), true);

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* indicator = root->findChild<QQuickItem*>(QStringLiteral("workingIndicator"));
    QVERIFY(indicator);
    QTRY_VERIFY(indicator->isVisible());

    auto* transcript = findVisualItem(root, "agentTranscript");
    auto* composer = findVisualItem(root, "agentComposer");
    QVERIFY(transcript && composer);
    QTRY_VERIFY(composer->mapToScene(QPointF{}).y() -
                    transcript->mapToScene(QPointF(0, transcript->height())).y() >=
                20);

    const auto commandRows = [&] {
        QList<QQuickItem*> result;
        const auto collect = [&result](const auto& self, QQuickItem* item) -> void {
            if (item->property("commandRow").isValid() && item->property("commandRow").toBool() &&
                item->isVisible())
                result.append(item);
            for (auto* child : item->childItems())
                self(self, child);
        };
        collect(collect, root);
        return result;
    };
    QTRY_COMPARE(commandRows().size(), 3);
    const auto rows = commandRows();
    const auto* last =
        *std::max_element(rows.cbegin(), rows.cend(), [](const auto* left, const auto* right) {
            return left->mapToScene(QPointF{}).y() < right->mapToScene(QPointF{}).y();
        });
    const qreal commandBottom = last->mapToScene(QPointF(0, last->height())).y();
    const qreal indicatorTop = indicator->mapToScene(QPointF{}).y();
    const auto* surface = last->findChild<QQuickItem*>(QStringLiteral("messageSurface"));
    QVERIFY(surface);
    QVERIFY(qAbs(indicator->mapToScene(QPointF{}).x() - surface->mapToScene(QPointF{}).x()) <= 4.0);
    QVERIFY2(commandBottom <= indicatorTop,
             qPrintable(QStringLiteral("Working indicator starts at %1 but the last command ends "
                                       "at %2.")
                            .arg(indicatorTop)
                            .arg(commandBottom)));
}

void AgentWorkspaceViewTest::streamingResponseStaysAnchoredAboveRunStatus() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));

    const QVariantMap user{{QStringLiteral("kind"), QStringLiteral("user")},
                           {QStringLiteral("body"), QStringLiteral("Please inspect this UI.")},
                           {QStringLiteral("meta"), QStringLiteral("You")},
                           {QStringLiteral("filePath"), QString()}};
    QVariantMap response{{QStringLiteral("kind"), QStringLiteral("assistant")},
                         {QStringLiteral("body"), QString()},
                         {QStringLiteral("meta"), QStringLiteral("Codex · streaming")},
                         {QStringLiteral("filePath"), QString()}};
    view.resetMessages({user}, QStringLiteral("stream-anchor"));
    view.setSessionState(QStringLiteral("Streaming"), QStringLiteral("Working…"), true);
    view.appendMessage(response);

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>(QStringLiteral("agentTranscript"));
    QVERIFY(transcript);
    QTRY_VERIFY(transcript->property("followTail").toBool());

    QString streamedBody;
    for (int paragraph = 0; paragraph < 80; ++paragraph) {
        streamedBody += QStringLiteral("Streaming paragraph %1 remains readable while more "
                                       "content arrives.\n\n")
                            .arg(paragraph + 1);
    }
    response.insert(QStringLiteral("body"), streamedBody);
    view.updateMessage(1, response);
    QTest::qWait(200);

    QTRY_VERIFY(transcript->property("followTail").toBool());
    QTRY_VERIFY(transcript->property("atYEnd").toBool());
    const auto endY = [&] {
        return transcript->property("originY").toDouble() +
               qMax(qreal(0),
                    transcript->property("contentHeight").toDouble() - transcript->height());
    };
    for (const int distance : {4, 31, 33, 80}) {
        transcript->setProperty("contentY", endY() - distance);
        QTRY_VERIFY(qAbs(endY() - transcript->property("contentY").toDouble() - distance) < 0.5);
        QVariant nearTail;
        QVERIFY(
            QMetaObject::invokeMethod(transcript, "nearTail", Q_RETURN_ARG(QVariant, nearTail)));
        QCOMPARE(nearTail.toBool(), distance <= 32);
    }
    transcript->setProperty("contentY", endY());
    for (int update = 0; update < 4; ++update) {
        // Keep following across consecutive coalesced streaming updates.
        streamedBody += QStringLiteral("More streamed content.\n\n").repeated(20);
        response.insert(QStringLiteral("body"), streamedBody);
        view.updateMessage(1, response);
        QTRY_VERIFY(transcript->property("followTail").toBool());
        QTRY_VERIFY(transcript->property("atYEnd").toBool());
    }

    auto* indicator = root->findChild<QQuickItem*>(QStringLiteral("workingIndicator"));
    QVERIFY(indicator);
    QVERIFY(indicator->isVisible());
    QList<QQuickItem*> bodies;
    QTRY_VERIFY(indicator->mapToScene(QPointF(0, indicator->height())).y() <=
                transcript->mapToScene(QPointF(0, transcript->height())).y() + 1);
    const auto collectBodies = [&bodies](const auto& self, QQuickItem* item) -> void {
        if (item->objectName() == QStringLiteral("messageBody") && item->isVisible())
            bodies.append(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    collectBodies(collectBodies, root);
    QVERIFY(!bodies.isEmpty());
    const auto* lastBody =
        *std::max_element(bodies.cbegin(), bodies.cend(), [](const auto* left, const auto* right) {
            return left->mapToScene(QPointF(0, left->height())).y() <
                   right->mapToScene(QPointF(0, right->height())).y();
        });
    const qreal responseBottom = lastBody->mapToScene(QPointF(0, lastBody->height())).y();
    const qreal indicatorTop = indicator->mapToScene(QPointF{}).y();
    QVERIFY2(responseBottom <= indicatorTop,
             qPrintable(QStringLiteral("Working indicator starts at %1 but streamed text ends "
                                       "at %2.")
                            .arg(indicatorTop)
                            .arg(responseBottom)));
}

void AgentWorkspaceViewTest::composerPlaceholderTracksCaret() {
    ketplus::ThemeManager theme(*qApp);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(theme.palette());
    view.setProductSidebarVisible(false);
    view.setAgentAvailable(true);
    view.setSessionState("Polish the workspace", "Ready", false);
    view.setConversations(
        {QVariantMap{{"id", "ui"},
                     {"active", true},
                     {"number", "42"},
                     {"tags", QVariantList{QVariantMap{{"label", "UI"}, {"color", "#3377cc"}}}}}});
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* input = root->findChild<QQuickItem*>("agentPrompt");
    auto* placeholder = root->findChild<QQuickItem*>("composerPlaceholder");
    QVERIFY(input && placeholder);
    auto* tag = root->findChild<QQuickItem*>("conversationTag_UI");
    QSignalSpy filtered(&view, &ketplus::AgentWorkspaceView::tagFilterRequested);
    QTRY_VERIFY((tag = findVisualItem(root, "conversationTag_UI")));
    QVERIFY(QMetaObject::invokeMethod(tag, "clicked"));
    QCOMPARE(filtered.count(), 1);
    QCOMPARE(filtered.first().first().toString(), QStringLiteral("UI"));
    QTRY_VERIFY(placeholder->isVisible());
    QTest::qWait(30);
    QRectF caret;
    QVERIFY(QMetaObject::invokeMethod(input, "positionToRectangle", Q_RETURN_ARG(QRectF, caret),
                                      Q_ARG(int, 0)));
    QTRY_VERIFY(qAbs(placeholder->y() - caret.y()) < 0.5);
    input->setProperty("text", "Draft");
    QTRY_VERIFY(!placeholder->isVisible());
    input->setProperty("text", "");
    QTRY_VERIFY(placeholder->isVisible());
    const auto capture = qEnvironmentVariable("KETPLUS_COMPOSER_CORE_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(30);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}

void AgentWorkspaceViewTest::composerFormattingSettles() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto* prompt = view.quickView()->rootObject()->findChild<QQuickItem*>("agentPrompt");
    QVERIFY(prompt);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(prompt->property("textDocument"));
    QVERIFY(quickDocument);
    auto* document = quickDocument->textDocument();
    const qreal pixelSize = prompt->property("font").value<QFont>().pixelSize();
    const auto verifyHeight = [&](qreal size) {
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            QCOMPARE(block.blockFormat().lineHeightType(), QTextBlockFormat::FixedHeight);
            QCOMPARE(block.blockFormat().lineHeight(),
                     qFloor(size * ket::Profile::current().composerLineHeight));
        }
    };

    prompt->setProperty("text", QStringLiteral("First line\nSecond line"));
    QTRY_COMPARE(document->lastBlock().blockFormat().lineHeight(),
                 qFloor(pixelSize * ket::Profile::current().composerLineHeight));
    verifyHeight(pixelSize);
    QSignalSpy documentChanges(document, &QTextDocument::contentsChanged);
    QSignalSpy textChanges(prompt, SIGNAL(textChanged()));
    QVERIFY(textChanges.isValid());
    QTest::qWait(40);
    QCOMPARE(documentChanges.count(), 0);
    QCOMPARE(textChanges.count(), 0);

    // Direct repeated calls must also leave revision/signals alone.
    const auto revision = document->revision();
    view.formatComposerDocument(quickDocument, pixelSize);
    view.formatComposerDocument(quickDocument, pixelSize);
    QCOMPARE(document->revision(), revision);
    QCOMPARE(documentChanges.count(), 0);

    // A formatting-only notification must not schedule another QML timer.
    auto* timer = prompt->findChild<QObject*>("composerLineHeightTimer");
    QVERIFY(timer);
    QSignalSpy formattingRuns(timer, SIGNAL(triggered()));
    QVERIFY(formattingRuns.isValid());
    QTextCursor formattingCursor(document->firstBlock());
    auto blockFormat = formattingCursor.blockFormat();
    blockFormat.setTopMargin(1);
    formattingCursor.setBlockFormat(blockFormat);
    QVERIFY(documentChanges.count() > 0);
    QVERIFY(textChanges.count() > 0);
    QTest::qWait(40);
    QCOMPARE(formattingRuns.count(), 0);

    // A new paragraph and a font change still receive the expected formatting.
    QTextCursor cursor(document);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\nThird line"));
    QTRY_COMPARE(document->lastBlock().blockFormat().lineHeight(),
                 qFloor(pixelSize * ket::Profile::current().composerLineHeight));
    verifyHeight(pixelSize);
    document->undo();
    QTRY_COMPARE(document->toPlainText(), QStringLiteral("First line\nSecond line"));

    auto font = prompt->property("font").value<QFont>();
    font.setPixelSize(static_cast<int>(pixelSize) + 2);
    prompt->setProperty("font", font);
    QTRY_COMPARE(document->firstBlock().blockFormat().lineHeight(),
                 qFloor(font.pixelSize() * ket::Profile::current().composerLineHeight));
    verifyHeight(font.pixelSize());
    documentChanges.clear();
    textChanges.clear();
    QTest::qWait(40);
    QCOMPARE(documentChanges.count(), 0);
    QCOMPARE(textChanges.count(), 0);
}

void AgentWorkspaceViewTest::formatsStreamingText() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    auto message = QVariantMap{{"kind", "assistant"},
                               {"meta", "Agent"},
                               {"filePath", ""},
                               {"body", "First paragraph.\n\nSecond **bold** paragraph."}};
    view.resetMessages({message});
    QQuickItem* body = nullptr;
    const auto findBody = [&](const auto& self, QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == "messageBody")
            return item;
        for (auto* child : item->childItems()) {
            if (auto* found = self(self, child))
                return found;
        }
        return nullptr;
    };
    QTRY_VERIFY((body = findBody(findBody, view.quickView()->rootObject())));
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(quickDocument);
    auto* document = quickDocument->textDocument();
    const qreal lineHeight = body->property("font").value<QFont>().pixelSize() * 1.45;
    QTRY_COMPARE(document->firstBlock().blockFormat().lineHeight(), lineHeight);
    QCOMPARE(document->firstBlock().blockFormat().lineHeightType(), QTextBlockFormat::FixedHeight);
    QCOMPARE(document->firstBlock().blockFormat().bottomMargin(), 10.0);
    QCOMPARE(document->toPlainText(), QStringLiteral("First paragraph.\nSecond bold paragraph."));
    QVERIFY(body->property("selectByMouse").toBool());
    message["body"] = "First paragraph.\n\nSecond **bold** paragraph.\n\nStreaming addition.";
    view.updateMessage(0, message);
    QTRY_VERIFY(document->toPlainText().endsWith("Streaming addition."));
    QTRY_COMPARE(document->lastBlock().blockFormat().lineHeight(), lineHeight);
    QCOMPARE(document->lastBlock().blockFormat().bottomMargin(), 10.0);

    message["body"] = "Intro.\n\n## Heading\n\nParagraph.\n\n- First\n- Second\n\nLast.";
    view.updateMessage(0, message);
    QTRY_COMPARE(document->blockCount(), 6);
    const QTextBlock intro = document->begin();
    const QTextBlock heading = intro.next();
    const QTextBlock paragraph = heading.next();
    const QTextBlock firstListItem = paragraph.next();
    const QTextBlock secondListItem = firstListItem.next();
    QCOMPARE(intro.blockFormat().bottomMargin(), 10.0);
    QCOMPARE(heading.blockFormat().topMargin(), 14.0);
    QCOMPARE(heading.blockFormat().bottomMargin(), 8.0);
    QCOMPARE(paragraph.blockFormat().bottomMargin(), 10.0);
    QCOMPARE(firstListItem.blockFormat().bottomMargin(), 4.0);
    QCOMPARE(secondListItem.blockFormat().bottomMargin(), 10.0);

    message["body"] = "| Name | State |\n| --- | --- |\n| Desktop | Ready |";
    view.updateMessage(0, message);
    const auto tableBorderColor = [&]() {
        for (auto* frame : document->rootFrame()->childFrames()) {
            if (auto* table = qobject_cast<QTextTable*>(frame))
                return table->format().borderBrush().color();
        }
        return QColor{};
    };
    QTRY_COMPARE(tableBorderColor(), QColor(testPalette().border));

    auto lightPalette = testPalette();
    lightPalette.dark = false;
    lightPalette.border = QStringLiteral("#e0e3e9");
    view.applyTheme(lightPalette);
    QTRY_COMPARE(tableBorderColor(), QColor(lightPalette.border));

    QTemporaryDir images;
    QImage sample(600, 200, QImage::Format_RGB32);
    sample.fill(Qt::green);
    const auto imagePath = images.filePath("test image.png");
    QVERIFY(sample.save(imagePath));
    message["body"] = QStringLiteral("![Preview](<%1>)").arg(imagePath);
    view.updateMessage(0, message);
    QTRY_COMPARE(document->firstBlock().blockFormat().lineHeightType(),
                 QTextBlockFormat::MinimumHeight);
    QTextCursor imageCursor(document);
    imageCursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    QTRY_VERIFY(imageCursor.charFormat().isImageFormat());
    const auto imageFormat = imageCursor.charFormat().toImageFormat();
    QVERIFY(imageFormat.width() <= body->width());
    QVERIFY(imageFormat.width() <= 88.0);
    QVERIFY(imageFormat.height() <= 88.0);
    QCOMPARE(imageFormat.height(), imageFormat.width() / 3);
    QCOMPARE(view.messageImageAt(quickDocument, 0).toLocalFile(), imagePath);
    QTest::qWait(100);
    const auto rendered = body->grabToImage();
    QVERIFY(rendered);
    QTRY_VERIFY(!rendered->image().isNull());
    int greenPixels = 0;
    const auto pixels = rendered->image();
    for (int y = 0; y < pixels.height(); ++y) {
        for (int x = 0; x < pixels.width(); ++x) {
            if (pixels.pixelColor(x, y) == QColor(Qt::green))
                ++greenPixels;
        }
    }
    QVERIFY(greenPixels > 1000);
    QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                      body->mapToScene(QPointF(10, 15)).toPoint());
    auto* popup = view.quickView()->rootObject()->findChild<QObject*>("imagePreview");
    QVERIFY(popup);
    QTRY_VERIFY(popup->property("visible").toBool());
    QCOMPARE(popup->property("imageSource").toUrl().toLocalFile(), imagePath);
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    message["kind"] = "user";
    message["body"] = "Inspect this image";
    message["images"] = QStringList{QUrl::fromLocalFile(imagePath).toString()};
    view.resetMessages({message});
    const auto findImage = [&](const auto& self, QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == "messageAttachmentImage")
            return item;
        for (auto* child : item->childItems()) {
            if (auto* found = self(self, child))
                return found;
        }
        return nullptr;
    };
    QQuickItem* attachment = nullptr;
    auto* activeTranscript =
        view.quickView()->rootObject()->findChild<QQuickItem*>(QStringLiteral("agentTranscript"));
    QVERIFY(activeTranscript);
    QTRY_VERIFY((attachment = findImage(findImage, activeTranscript)));
    QTRY_COMPARE(attachment->property("status").toInt(), 1);
    QCOMPARE(attachment->property("source").toUrl().toLocalFile(), imagePath);
    QCOMPARE(attachment->parentItem()->width(), 88.0);
    QCOMPARE(attachment->parentItem()->height(), 88.0);
    QTest::qWait(150);
    QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                      attachment->mapToScene(QPointF(20, 20)).toPoint());
    QTRY_VERIFY(popup->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    QTRY_VERIFY(!popup->property("visible").toBool());

    message["images"] = QStringList(4, QUrl::fromLocalFile(imagePath).toString());
    view.resetMessages({message});
    QQuickItem* gallery = nullptr;
    QTRY_VERIFY((gallery = findVisualItem(view.quickView()->rootObject(),
                                          QStringLiteral("messageImageGallery"))));
    QTRY_COMPARE(gallery->property("columns").toInt(), 4);
    auto* moreImages = findVisualItem(gallery, "expandMessageImages");
    QVERIFY(moreImages && moreImages->isVisible());
    QCOMPARE(moreImages->property("text").toString(), QString("+1"));
    QVERIFY(QMetaObject::invokeMethod(moreImages, "clicked"));
    QTRY_VERIFY(!moreImages->isVisible());
    gallery->setWidth(200); // An expanded gallery wraps when the column narrows.
    QTRY_COMPARE(gallery->property("columns").toInt(), 2);
    QTRY_COMPARE(gallery->height(), 182.0);
    QList<QQuickItem*> thumbnails;
    const auto collectImages = [&](const auto& self, QQuickItem* item) -> void {
        if (item->objectName() == "messageAttachmentImage")
            thumbnails.append(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    QTRY_VERIFY(([&] {
        thumbnails.clear();
        collectImages(collectImages, gallery);
        return thumbnails.size() == 4;
    })());
    for (auto* thumbnail : thumbnails) {
        QVERIFY(thumbnail->width() <= 88.0);
        QVERIFY(thumbnail->height() <= 88.0);
    }
    // The row is reused rather than rebuilt when its images change, so the grid
    // re-positions its cells on the next polish instead of at creation.
    QTRY_COMPARE(thumbnails.at(0)->mapToItem(gallery, QPointF()).y(),
                 thumbnails.at(1)->mapToItem(gallery, QPointF()).y());
    QTRY_VERIFY(thumbnails.at(3)->mapToItem(gallery, QPointF()).y() >
                thumbnails.at(0)->mapToItem(gallery, QPointF()).y());

    // Restore the explicit test width before reusing this delegate for an assistant.
    gallery->setWidth(370);
    message["kind"] = "assistant";
    view.resetMessages({message});
    QQuickItem* assistantGallery = nullptr;
    QTRY_VERIFY((assistantGallery = findVisualItem(view.quickView()->rootObject(),
                                                   QStringLiteral("messageImageGallery"))));
    QCOMPARE(assistantGallery->property("columns").toInt(), 4);
    QVERIFY(assistantGallery->width() <= 370.0);
}

void AgentWorkspaceViewTest::linksInlineCodeOnceFilesAreListed() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    QVERIFY(QDir(root).mkpath(QStringLiteral("src")));
    QFile file(QDir(root).filePath(QStringLiteral("src/unique.ts")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QProcess git;
    git.start(QStringLiteral("git"),
              {QStringLiteral("-C"), root, QStringLiteral("init"), QStringLiteral("-q")});
    QVERIFY(git.waitForFinished(10'000));

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.setActivePath(root);
    view.show();
    // Formatting never lists the tree, so the suffix links only once the background listing
    // lands and the shown message is formatted again.
    view.resetMessages({QVariantMap{
        {"kind", "assistant"}, {"meta", "Agent"}, {"body", "Edit `unique.ts` next."}}});
    auto* body = findVisualItem(view.quickView()->rootObject(), QStringLiteral("messageBody"));
    QTRY_VERIFY(body);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(quickDocument);
    const auto linkHref = [&view, quickDocument] {
        QTextDocument* document = quickDocument->textDocument();
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                if (it.fragment().charFormat().isAnchor())
                    return view.messageLinkAt(quickDocument, it.fragment().position());
            }
        }
        return QString{};
    };
    QTRY_COMPARE_WITH_TIMEOUT(linkHref(), QStringLiteral("src/unique.ts"), 5000);
}

void AgentWorkspaceViewTest::opensInlineMarkdownLinks() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.show();
    view.resetMessages({QVariantMap{{"kind", "assistant"},
                                    {"meta", "Agent"},
                                    {"body", "Open [models.ts](models.ts) from this message."}}});

    auto* body = findVisualItem(view.quickView()->rootObject(), QStringLiteral("messageBody"));
    QTRY_VERIFY(body);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(quickDocument);
    auto* document = quickDocument->textDocument();
    QTRY_VERIFY(document);

    QTextFragment link;
    for (auto it = document->firstBlock().begin(); !it.atEnd(); ++it) {
        if (it.fragment().charFormat().isAnchor()) {
            link = it.fragment();
            break;
        }
    }
    QVERIFY(link.isValid());
    QCOMPARE(view.messageLinkAt(quickDocument, link.position()), QStringLiteral("models.ts"));
    const QTextBlock block = document->findBlock(link.position());
    QVERIFY(block.isValid());
    const QTextLine line = block.layout()->lineForTextPosition(link.position() - block.position());
    QVERIFY(line.isValid());
    const QRectF blockRect = document->documentLayout()->blockBoundingRect(block);
    const QPointF linkPoint(blockRect.left() +
                                line.cursorToX(link.position() - block.position() + 1),
                            blockRect.top() + line.y() + line.height() / 2);

    auto* linkHover = body->findChild<QObject*>("messageBodyLinkHover");
    QVERIFY(linkHover);
    view.quickView()->requestActivate();
    QTest::qWait(50);
    QTest::mouseMove(view.quickView(), QPoint(0, 0));
    QTest::mouseMove(view.quickView(), body->mapToScene(linkPoint).toPoint());
    QTRY_COMPARE(linkHover->property("cursorShape").toInt(), int(Qt::PointingHandCursor));
    QTest::mouseMove(view.quickView(), body->mapToScene(QPointF(4, linkPoint.y())).toPoint());
    QTRY_COMPARE(linkHover->property("cursorShape").toInt(), int(Qt::IBeamCursor));

    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::fileOpenRequested);
    QTest::qWait(100);
    QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                      body->mapToScene(linkPoint).toPoint());
    QTRY_COMPARE(opened.count(), 1);
    QCOMPARE(opened.constFirst().constFirst().toString(), QStringLiteral("models.ts"));
}

void AgentWorkspaceViewTest::conversationColumnFollowsConfiguredWidth() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1500, 900);
    view.show();
    view.resetMessages({QVariantMap{{"kind", "assistant"},
                                    {"meta", "Claude"},
                                    {"body", QString("A long answer. ").repeated(80)}}},
                       "conversation-width");
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QTRY_COMPARE(root->findChild<QQuickItem*>("agentTranscript")->opacity(), 1.0);
    auto* composer = root->findChild<QQuickItem*>("agentComposer");
    QVERIFY(composer);
    const auto bodyWidth = [root] {
        auto* body = findVisualItem(root, "messageBody");
        return body ? body->width() : -1.0;
    };
    const qreal zoom = view.tokens().value("zoomFactor", 1.0).toReal();
    const qreal defaultWidth = bodyWidth();
    QVERIFY(defaultWidth > 0);
    QVERIFY(defaultWidth < ketplus::AgentWorkspaceView::defaultConversationWidth * zoom);
    QCOMPARE(composer->width(),
             std::round(ketplus::AgentWorkspaceView::defaultConversationWidth * zoom) -
                 std::round(40 * zoom));

    view.setConversationWidth(1200);
    QTRY_VERIFY(bodyWidth() > defaultWidth + 400 * zoom);
    QCOMPARE(composer->width(), std::round(1200 * zoom) - std::round(40 * zoom));

    // Zero fills the pane: wider than any fixed column on this 1500px view.
    view.setConversationWidth(0);
    QTRY_VERIFY(bodyWidth() > 1200 * zoom);
    QVERIFY(composer->width() > 1200 * zoom);
}

void AgentWorkspaceViewTest::markdownHeadingsAndInlineCodeStayNearBodySize() {
    ketplus::AgentWorkspaceView view;
    const auto palette = testPalette();
    view.applyTheme(palette);
    view.resize(900, 900);
    view.show();
    const QString source = "# Heading one\n\n## Heading two\n\n### Heading three\n\n"
                           "Body with `inline_code` and **strong** text.";
    view.resetMessages({QVariantMap{{"kind", "assistant"}, {"meta", "Claude"}, {"body", source}}},
                       "markdown-typography");
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* body = findVisualItem(root, "messageBody");
    QVERIFY(body);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(quickDocument);
    auto* document = quickDocument->textDocument();
    const int bodyPixels = body->property("font").value<QFont>().pixelSize();
    QVERIFY(bodyPixels > 0);

    const auto firstFormat = [](const QTextBlock& block) {
        return block.begin().fragment().charFormat();
    };
    auto block = document->firstBlock();
    const QList<qreal> scales{1.25, 1.125, 1.0};
    for (int level = 1; level <= 3; ++level, block = block.next()) {
        QCOMPARE(block.blockFormat().headingLevel(), level);
        const auto format = firstFormat(block);
        const int expected = qRound(bodyPixels * scales.at(level - 1));
        // Qt's importer would leave a +3..+1 size adjustment that doubles an H1.
        QCOMPARE(format.intProperty(QTextFormat::FontSizeAdjustment), 0);
        QCOMPARE(format.intProperty(QTextFormat::FontPixelSize), expected);
        QCOMPARE(format.fontWeight(), int(QFont::DemiBold));
        QCOMPARE(block.blockFormat().lineHeight(), std::ceil(expected * 1.3));
    }

    QString code;
    bool sawStrong = false;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        const auto format = fragment.charFormat();
        if (format.fontFixedPitch()) {
            code += fragment.text();
            QCOMPARE(format.intProperty(QTextFormat::FontPixelSize),
                     qRound(bodyPixels * 0.875) - 1);
            // Code keeps the body colour; a rounded chip drawn behind it sets it apart.
            QVERIFY(!format.hasProperty(QTextFormat::ForegroundBrush));
            QVERIFY(!format.hasProperty(QTextFormat::BackgroundBrush));
            QVERIFY(format.fontFixedPitch());
        } else if (fragment.text() == "strong") {
            sawStrong = true;
            QCOMPARE(format.fontWeight(), int(QFont::DemiBold));
        }
    }
    QCOMPARE(code, QStringLiteral("inline_code"));
    QVERIFY(sawStrong);
    // The space before the code and the code's last character are tracked by the
    // chip padding, so the chip leaves the neighbouring text uncovered.
    const int codeStart = document->toPlainText().indexOf("inline_code");
    QVERIFY(codeStart > 0);
    for (const int position : {codeStart - 1, codeStart + 10}) {
        QTextCursor cursor(document);
        cursor.setPosition(position + 1);
        QCOMPARE(cursor.charFormat().fontLetterSpacingType(), QFont::AbsoluteSpacing);
        QCOMPARE(cursor.charFormat().fontLetterSpacing(), 4.0);
    }
    QTextCursor inside(document);
    inside.setPosition(codeStart + 5);
    QVERIFY(inside.charFormat().fontLetterSpacing() == 0.0);

    // One chip, hugging the code's glyphs with a little room on each side.
    const auto chips = view.messageInlineCode(quickDocument);
    QCOMPARE(chips.size(), 1);
    const auto chip = chips.first().toMap();
    QRectF before;
    QVERIFY(QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, before),
                                      Q_ARG(int, codeStart - 1)));
    QRectF first;
    QRectF last;
    QVERIFY(QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, first),
                                      Q_ARG(int, codeStart)));
    QVERIFY(QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, last),
                                      Q_ARG(int, codeStart + 11)));
    QCOMPARE(chip.value("x").toDouble(), first.x() - 4);
    // The right edge already includes the last character's tracking.
    QCOMPARE(chip.value("width").toDouble(), last.x() - first.x() + 4);
    QVERIFY(chip.value("x").toDouble() > before.x());
    const double top = chip.value("y").toDouble() + body->property("topPadding").toDouble();
    const double bottom = top + chip.value("height").toDouble();
    // The chip wraps the smaller code glyphs plus padding, so it stays within a
    // couple of pixels of the body text's own line box.
    QVERIFY2(top >= first.y() - 2 && bottom <= first.y() + first.height() + 2,
             qPrintable(QStringLiteral("chip %1..%2 outside line %3..%4")
                            .arg(top)
                            .arg(bottom)
                            .arg(first.y())
                            .arg(first.y() + first.height())));
    QTRY_VERIFY(findVisualItem(body, "messageInlineCodeChip") != nullptr);
    if (const auto capturePath = qEnvironmentVariable("KETPLUS_INLINE_CODE_CAPTURE");
        !capturePath.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capturePath));
    }

    // Formatting again must leave the document as it is.
    const auto height = body->height();
    view.formatMessageDocument(quickDocument, bodyPixels, body->width());
    QCOMPARE(body->height(), height);
}

void AgentWorkspaceViewTest::reopenedRowFormatsItsDocumentOnce() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 900);
    view.show();
    const QVariantList answer{QVariantMap{{"kind", "assistant"},
                                          {"meta", "Codex"},
                                          {"messageKey", "answer"},
                                          {"body", "## Result\n\nText with `code`.\n\n```cpp\nint "
                                                   "value = 1;\n```\n"}}};
    const QVariantList other{QVariantMap{
        {"kind", "assistant"}, {"meta", "Codex"}, {"messageKey", "other"}, {"body", "Other."}}};
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    const auto formatsOf = [&]() {
        auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
        if (!transcript || transcript->opacity() != 1.0)
            return -1;
        auto* body = findVisualItem(root, "messageBody");
        return body && body->property("sourceInitialized").toBool()
                   ? body->property("paragraphFormats").toInt()
                   : -1;
    };
    // The first row may format again while the new window is still sized.
    // Returning to the conversation recreates the row in a settled view, where
    // applying its source formats the document exactly once.
    view.resetMessages(answer, "format-once:a");
    QTRY_VERIFY(formatsOf() >= 1);
    view.resetMessages(other, "format-once:b");
    QTRY_VERIFY(formatsOf() >= 1);
    view.resetMessages(answer, "format-once:a");
    QTRY_COMPARE(formatsOf(), 1);
    QTest::qWait(100);
    QCOMPARE(formatsOf(), 1);
    QTRY_VERIFY(findVisualItem(root, "messageCodeBlockFrame") != nullptr);
}

void AgentWorkspaceViewTest::compactCodeBlocks_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark-wide") << 1000 << true;
    QTest::newRow("dark-narrow") << 600 << true;
    QTest::newRow("light-wide") << 1000 << false;
}

void AgentWorkspaceViewTest::compactCodeBlocks() {
    QFETCH(int, width);
    QFETCH(bool, dark);
    ketplus::AgentWorkspaceView view;
    auto palette = testPalette();
    if (!dark) {
        palette.dark = false;
        palette.pageBackground = "#ffffff";
        palette.surfaceRaised = "#eef1f4";
        palette.borderStrong = "#a1a8b0";
        palette.textMain = "#20252b";
    }
    view.applyTheme(palette);
    view.resize(width, 900);
    view.show();
    const QString source =
        "Example command:\n\n```sh\n# Build\ncmake --build --preset dev\n\nctest --preset dev "
        "--output-on-failure\n```\n\n"
        "Separate snippet:\n\n~~~cpp\nint main() {\n    return 0;\n}\n~~~\n\nAfter the snippets.";
    QVariantMap message{{"kind", "assistant"}, {"meta", "Codex"}, {"body", source}};
    view.resetMessages({message}, "code-blocks");
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    auto* body = findVisualItem(root, "messageBody");
    QVERIFY(body);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(body->property("textDocument"));
    QVERIFY(quickDocument);
    auto* document = quickDocument->textDocument();
    const auto plain = document->toPlainText();
    auto frames = view.messageCodeBlocks(quickDocument);
    QCOMPARE(frames.size(), 2);
    auto code = document->firstBlock();
    while (code.isValid() && !code.blockFormat().hasProperty(QTextFormat::BlockCodeLanguage))
        code = code.next();
    QVERIFY(code.isValid());
    const auto codeFont =
        code.begin().fragment().charFormat().intProperty(QTextFormat::FontPixelSize);
    // The transcript reads a step above the interface size, so the expectations
    // follow the body rather than a fixed pixel size.
    const int bodyPixels = body->property("font").value<QFont>().pixelSize();
    QVERIFY(bodyPixels > 0);
    QCOMPARE(codeFont, bodyPixels - 2);
    // Code blocks already sit in a frame; they get no inline chips.
    QVERIFY(view.messageInlineCode(quickDocument).isEmpty());
    QCOMPARE(document->firstBlock().blockFormat().lineHeight(), bodyPixels * 1.45);
    QCOMPARE(code.blockFormat().lineHeight(), std::ceil((bodyPixels - 2) * 1.45));
    QCOMPARE(code.next().blockFormat().bottomMargin(), 0.0);
    const auto height = body->height();
    view.formatMessageDocument(quickDocument, bodyPixels, body->width());
    QCOMPARE(document->toPlainText(), plain);
    QCOMPARE(view.messageCodeBlocks(quickDocument), frames);
    QCOMPARE(body->height(), height);
    for (const auto& value : frames) {
        const auto frame = value.toMap();
        QVERIFY2(frame.value("x").toDouble() >= -1,
                 qPrintable(QJsonDocument::fromVariant(frame).toJson()));
        QVERIFY(frame.value("y").toDouble() >= 0);
        QVERIFY(frame.value("x").toDouble() + frame.value("width").toDouble() <= body->width() + 1);
        QVERIFY(frame.value("y").toDouble() + frame.value("height").toDouble() <=
                body->height() + 1);
    }
    QVERIFY(QMetaObject::invokeMethod(body, "selectAll"));
    QCOMPARE(body->property("selectedText").toString().replace(QChar::ParagraphSeparator, '\n'),
             plain);
    QVERIFY(QMetaObject::invokeMethod(body, "copy"));
    QCOMPARE(QGuiApplication::clipboard()->text(), plain);
    QVERIFY(QMetaObject::invokeMethod(body, "deselect"));
    const auto capturePath = qEnvironmentVariable("KETPLUS_SNIPPET_CAPTURE");
    if (!capturePath.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(100);
        const auto image = view.quickView()->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(capturePath + "." + QTest::currentDataTag() + ".png"));
        const auto first = frames.first().toMap();
        const auto origin = body->mapToScene(
            QPointF(first.value("x").toDouble() + 4,
                    first.value("y").toDouble() + first.value("height").toDouble() / 2));
        const auto scale = image.devicePixelRatio();
        QCOMPARE(image.pixelColor(qRound(origin.x() * scale), qRound(origin.y() * scale)),
                 QColor(view.tokens().value("codeBlockBackground").toString()));
    }
    message["body"] = "```sh\nfirst line\n";
    view.updateMessage(0, message);
    QTRY_COMPARE(view.messageCodeBlocks(quickDocument).size(), 1);
    QTest::qWait(50);
    auto* standaloneFrame = findVisualItem(body, "messageCodeBlockFrame");
    QVERIFY(standaloneFrame);
    // Code blocks sit darker than the page in both themes, not on the raised surface.
    const QColor frameColor = standaloneFrame->property("color").value<QColor>();
    QCOMPARE(frameColor, QColor(view.tokens().value("codeBlockBackground").toString()));
    QVERIFY(frameColor.lightnessF() < QColor(palette.pageBackground).lightnessF());
    QVERIFY(standaloneFrame->y() >= 0);
    QVERIFY(standaloneFrame->y() + standaloneFrame->height() <= body->height() + 1);
    message["body"] = "```sh\nfirst line\nsecond line\n```\n\nDone.";
    view.updateMessage(0, message);
    QTRY_COMPARE(view.messageCodeBlocks(quickDocument).size(), 1);
    QTRY_COMPARE(document->lastBlock().blockFormat().lineHeight(), bodyPixels * 1.45);
    message["body"] = "Inline `code` stays in its paragraph.";
    view.updateMessage(0, message);
    QTRY_VERIFY(view.messageCodeBlocks(quickDocument).isEmpty());
    const QStringList paths{"/Users/developer/project/CMakeLists.txt",
                            "/Users/developer/project/src/agent/AgentMessageModel.cpp",
                            "/Users/developer/project/src/agent/AgentMessageModel.h",
                            "/Users/developer/project/src/agent/AgentWorkspaceView.cpp",
                            "/Users/developer/project/src/agent/AgentWorkspaceView.h"};
    QVariantList files;
    for (const auto& path : paths)
        files.append(QVariantMap{{"path", path}, {"known", true}, {"added", 12}, {"removed", 3}});
    view.resetMessages({QVariantMap{{"kind", "file"},
                                    {"meta", "5 file(s) - completed"},
                                    {"body", paths.join('\n')},
                                    {"fileChanges", files}}},
                       "compact-files");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    auto* surface = findVisualItem(root, "messageSurface");
    QVERIFY(surface);
    QVERIFY2(surface->height() <= (width > 600 ? 170 : 260),
             qPrintable(QString::number(surface->height())));
    if (!capturePath.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(capturePath + "." + QTest::currentDataTag() +
                                                    ".files.png"));
}

void AgentWorkspaceViewTest::pastesImagesAndOpensIndividualFiles() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1100, 900);
    view.show();
    view.quickView()->requestActivate();
    QTest::qWait(100);
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QGuiApplication::clipboard()->setText("plain text");
    QVERIFY(!view.pasteClipboardImage());
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::green);
    QGuiApplication::clipboard()->setImage(image);
    view.setSessionState("Task", "Working", true);
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    QVERIFY(prompt);
    prompt->forceActiveFocus();
    QTRY_VERIFY(prompt->hasActiveFocus());
    QTest::keyClick(view.quickView(), Qt::Key_V, Qt::ControlModifier);
    QTRY_COMPARE(view.attachments().size(), 1);
    const auto path = view.attachments().first().toMap().value("path").toString();
    QCOMPARE(QImage(path), image);
    view.removeAttachment(0);
    QFile::remove(path);

    QTemporaryDir worktreeParent;
    QVERIFY(worktreeParent.isValid());
    const QString worktree = worktreeParent.filePath(QStringLiteral("worktree-a"));
    QVERIFY(QDir().mkpath(worktree + QStringLiteral("/src")));
    const QString absoluteSource = worktree + QStringLiteral("/src/file-b.ts");
    QFile source(absoluteSource);
    QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Text));
    source.write("export {}\n");
    source.close();
    view.setActivePath(worktree);
    QCOMPARE(view.absoluteFilePath(QStringLiteral("src/file-b.ts")),
             QFileInfo(absoluteSource).canonicalFilePath());
    QCOMPARE(view.displayFilePath(absoluteSource), QStringLiteral("worktree-a/src/file-b.ts"));

    const QStringList paths{"src/first.cpp", "src/second.cpp", "src/third.cpp", "src/fourth.cpp"};
    QVariantList fileChanges;
    for (int index = 0; index < paths.size(); ++index) {
        QFile changed(QDir(worktree).filePath(paths[index]));
        QVERIFY(changed.open(QIODevice::WriteOnly));
        fileChanges.append(QVariantMap{{"path", paths[index]},
                                       {"added", index + 1},
                                       {"removed", index},
                                       {"known", index != 3}});
    }
    // A changed path that no longer exists on disk.
    const QString removedPath = QStringLiteral("src/removed.cpp");
    fileChanges.append(QVariantMap{{"path", removedPath}, {"known", false}});
    // A rename whose source is gone acts on the destination, which exists.
    fileChanges.append(QVariantMap{{"path", QStringLiteral("src/renamed-from.cpp")},
                                   {"destination", paths.first()},
                                   {"known", false}});
    view.resetMessages({QVariantMap{{"kind", "file"},
                                    {"meta", "4 updated files"},
                                    {"filePath", paths.first()},
                                    {"body", paths.join('\n')},
                                    {"fileChanges", fileChanges}}});
    auto* editActivity = findVisualItem(root, "transcriptActivityRow");
    QVERIFY(editActivity);
    QVERIFY(!editActivity->property("expanded").toBool());
    QVERIFY(QMetaObject::invokeMethod(editActivity, "toggleDetails"));
    QTRY_VERIFY(editActivity->property("expanded").toBool());
    QList<QQuickItem*> rows;
    const auto collect = [&](const auto& self, QQuickItem* item) -> void {
        if (item->objectName() == "changedFileRow")
            rows.append(item);
        for (auto* child : item->childItems())
            self(self, child);
    };
    QTRY_VERIFY(([&] {
        rows.clear();
        collect(collect, root);
        return rows.size() == 6;
    })());
    // The existence check runs off the GUI thread; only the removed path turns missing.
    QQuickItem* renamedRow = rows.takeLast();
    QQuickItem* removedRow = rows.takeLast();
    QTRY_VERIFY(removedRow->property("missing").toBool());
    QCOMPARE(
        removedRow->findChild<QObject*>("changedFileMouseArea")->property("cursorShape").toInt(),
        int(Qt::ArrowCursor));
    for (const auto* row : rows)
        QVERIFY(!row->property("missing").toBool());
    QCOMPARE(rows.constFirst()
                 ->findChild<QObject*>("changedFileMouseArea")
                 ->property("cursorShape")
                 .toInt(),
             int(Qt::PointingHandCursor));
    const auto* removedLabel = removedRow->findChild<QQuickItem*>("changedFileLabel");
    QVERIFY(removedLabel);
    QCOMPARE(removedLabel->property("color").value<QColor>(), QColor(testPalette().danger));
    QVERIFY(view.pathMissing(removedPath));
    QVERIFY(!renamedRow->property("missing").toBool());
    QCOMPARE(renamedRow->property("targetPath").toString(), paths.first());
    QVERIFY(!view.pathMissing(paths.first()));
    QTest::qWait(100);
    QSignalSpy opened(&view, &ketplus::AgentWorkspaceView::fileDiffRequested);
    for (int index = 0; index < rows.size(); ++index) {
        const auto* row = rows.at(index);
        QCOMPARE(row->height(), 24.0);
        auto* added = row->findChild<QQuickItem*>("fileAdditions");
        auto* removed = row->findChild<QQuickItem*>("fileDeletions");
        QVERIFY(added && removed);
        QCOMPARE(added->isVisible(), index != 3);
        QCOMPARE(removed->isVisible(), index != 3);
        QCOMPARE(added->property("text").toString(), "+" + QString::number(index + 1));
        QCOMPARE(removed->property("text").toString(), "-" + QString::number(index));
        QVERIFY(removed->mapToItem(row, QPointF(removed->width(), 0)).x() <= row->width() + 1);
        if (index > 0)
            QCOMPARE(row->y(), rows.at(index - 1)->y() + rows.at(index - 1)->height() + 2);
        QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                          row->mapToScene(QPointF(30, row->height() / 2)).toPoint());
        QCOMPARE(opened.size(), index + 1);
        QCOMPARE(opened.last().first().toString(), paths.at(index));
    }
    QTest::mouseClick(view.quickView(), Qt::LeftButton, Qt::NoModifier,
                      removedRow->mapToScene(QPointF(30, removedRow->height() / 2)).toPoint());
    QCOMPARE(opened.size(), paths.size());
    QTest::mouseClick(view.quickView(), Qt::RightButton, Qt::NoModifier,
                      removedRow->mapToScene(QPointF(30, 12)).toPoint());
    auto* removedMenu = removedRow->findChild<QObject*>("changedFileContextMenu");
    QVERIFY(removedMenu);
    QTRY_VERIFY(removedMenu->property("visible").toBool());
    QVERIFY(removedMenu->findChild<QObject*>("copyChangedFilePathAction")
                ->property("enabled")
                .toBool());
    QVERIFY(
        !removedMenu->findChild<QObject*>("revealChangedFileAction")->property("enabled").toBool());
    QVERIFY(
        !removedMenu->findChild<QObject*>("openChangedFileAction")->property("enabled").toBool());
    // The row's own menu replaces the transcript's conversation menu.
    auto* transcriptMenu = root->findChild<QObject*>("transcriptContextMenu");
    QVERIFY(transcriptMenu);
    QTest::qWait(50);
    QVERIFY(!transcriptMenu->property("visible").toBool());
    QMetaObject::invokeMethod(removedMenu, "close");
    QTRY_VERIFY(!removedMenu->property("visible").toBool());

    const auto* firstLabel = rows.constFirst()->findChild<QQuickItem*>("changedFileLabel");
    QVERIFY(firstLabel);
    QCOMPARE(firstLabel->property("text").toString(), QStringLiteral("worktree-a/src/first.cpp"));
    auto* fileTooltip = rows.constFirst()->findChild<QObject*>(QStringLiteral("ketToolTip"));
    QVERIFY(fileTooltip);
    QCOMPARE(fileTooltip->property("backgroundColor").value<QColor>(),
             ket::tooltipRecipe(ket::Appearance::instance().theme()).background);
    QCOMPARE(fileTooltip->property("labelColor").value<QColor>(),
             ket::tooltipRecipe(ket::Appearance::instance().theme()).text);
    QTest::mouseClick(view.quickView(), Qt::RightButton, Qt::NoModifier,
                      rows.constFirst()->mapToScene(QPointF(30, 12)).toPoint());
    auto* contextMenu = rows.constFirst()->findChild<QObject*>("changedFileContextMenu");
    QVERIFY(contextMenu);
    QTRY_VERIFY(contextMenu->property("visible").toBool());
    auto* copyAction = contextMenu->findChild<QObject*>("copyChangedFilePathAction");
    auto* openAction = contextMenu->findChild<QObject*>("openChangedFileAction");
    QVERIFY(copyAction);
    QVERIFY(contextMenu->findChild<QObject*>("revealChangedFileAction"));
    QVERIFY(openAction);
    QGuiApplication::clipboard()->clear();
    QVERIFY(QMetaObject::invokeMethod(copyAction, "triggered"));
    QCOMPARE(
        QGuiApplication::clipboard()->text(),
        QDir(QFileInfo(worktree).canonicalFilePath()).filePath(QStringLiteral("src/first.cpp")));
    QSignalSpy openedFile(&view, &ketplus::AgentWorkspaceView::fileOpenRequested);
    QVERIFY(QMetaObject::invokeMethod(openAction, "triggered"));
    QCOMPARE(openedFile.count(), 1);
    QCOMPARE(openedFile.constFirst().constFirst().toString(), QStringLiteral("src/first.cpp"));
}

void AgentWorkspaceViewTest::liveCodexSmoke() {
    if (!qEnvironmentVariableIsSet("KETPLUS_RUN_LIVE_AGENT_TEST")) {
        QSKIP("Set KETPLUS_RUN_LIVE_AGENT_TEST=1 to make an authenticated model call.");
    }

    QTemporaryDir worktree;
    QVERIFY(worktree.isValid());
    ketplus::AgentSessionManager manager;
    QVERIFY2(manager.isAvailable(), "Codex CLI is not installed or discoverable.");
    QSignalSpy models(&manager, &ketplus::AgentSessionManager::modelsLoaded);
    manager.loadModels();
    QTRY_COMPARE_WITH_TIMEOUT(models.size(), 1, 20000);
    QVERIFY(!models.first().first().toList().isEmpty());
    manager.submitPrompt(
        worktree.path(),
        QStringLiteral("Reply with exactly KETPLUS_LIVE_OK. Do not use tools or modify files."),
        {{"provider", "codex"},
         {"model", "gpt-5.6-luna"},
         {"effort", "medium"},
         {"permission", "read-only"}});
    QTRY_VERIFY_WITH_TIMEOUT(manager.isRunning(worktree.path()), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!manager.isRunning(worktree.path()), 120000);

    bool foundExpectedReply = false;
    for (const QVariant& value : manager.messages(worktree.path())) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("kind")).toString() == QStringLiteral("assistant") &&
            entry.value(QStringLiteral("body"))
                .toString()
                .contains(QStringLiteral("KETPLUS_LIVE_OK"))) {
            foundExpectedReply = true;
            break;
        }
    }
    QVERIFY2(foundExpectedReply, "The live Codex turn did not stream the expected reply.");
    QCOMPARE(manager.status(worktree.path()), QStringLiteral("Ready"));
}

void AgentWorkspaceViewTest::userBubbleFitsShortTextAndWrapsLongText() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    QVariantMap message{{"kind", "user"}, {"body", "OK"}, {"meta", "You"}};
    view.resetMessages({message});
    auto* root = view.quickView()->rootObject();
    QQuickItem* surface = nullptr;
    QTRY_VERIFY((surface = findVisualItem(root, "messageSurface")));
    QTRY_VERIFY(surface->width() > 0 && surface->width() < 160);
    const double shortWidth = surface->width();
    message["body"] = QString::fromUtf8("Thực hiện cả 2 mục vàng");
    view.updateMessage(0, message);
    auto* shortBody = findVisualItem(root, "messageBody");
    QVERIFY(shortBody);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(shortBody->property("textDocument"));
    QVERIFY(quickDocument);
    QTRY_COMPARE(quickDocument->textDocument()->begin().layout()->lineCount(), 1);
    QCOMPARE(shortBody->property("color").value<QColor>(), QColor(Qt::white));
    QCOMPARE(surface->property("color").value<QColor>(), QColor("#4f6fff").darker(135));
    const auto capture = qEnvironmentVariable("KETPLUS_USER_BUBBLE_CAPTURE");
    if (!capture.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
    const double right = surface->x() + surface->width();
    message["body"] =
        QString("A long message that must wrap inside the conversation column. ").repeated(40);
    view.updateMessage(0, message);
    QTRY_VERIFY(surface->width() > shortWidth);
    QVERIFY(surface->width() <= surface->property("userMaximumWidth").toDouble());
    QVERIFY(surface->width() <=
            surface->parentItem()->property("contentColumnWidth").toDouble() * 0.8 + 1);
    QTRY_VERIFY(qAbs(surface->x() + surface->width() - right) < 1);
    auto* body = findVisualItem(root, "messageBody");
    QVERIFY(body);
    QTRY_VERIFY(body->property("contentHeight").toDouble() > 30);
    QTRY_VERIFY(body->property("userOverlong").toBool());
    auto* showMore = findVisualItem(root, "userMessageShowMore");
    QVERIFY(showMore);
    QVERIFY(showMore->isVisible());
    const double collapsedHeight = body->height();
    QVERIFY(QMetaObject::invokeMethod(showMore, "clicked"));
    QTRY_VERIFY(body->height() > collapsedHeight);
    QVERIFY(QMetaObject::invokeMethod(showMore, "clicked"));
    QTRY_VERIFY(body->height() <= collapsedHeight + 1);
    auto* prompt = findVisualItem(root, "agentPrompt");
    QVERIFY(prompt);
    prompt->setProperty("text", "one\ntwo");
    auto* composer = qvariant_cast<QQuickTextDocument*>(prompt->property("textDocument"));
    QVERIFY(composer);
    const auto font = prompt->property("font").value<QFont>();
    QTRY_COMPARE(composer->textDocument()->begin().blockFormat().lineHeight(),
                 qFloor(font.pixelSize() * ket::Profile::current().composerLineHeight));
    auto* document = composer->textDocument();
    document->clearUndoRedoStacks();
    QTextCursor edit(document);
    edit.movePosition(QTextCursor::End);
    edit.insertText("!");
    view.formatComposerDocument(composer, font.pixelSize());
    document->undo();
    QCOMPARE(document->toPlainText(), QString("one\ntwo"));
}
