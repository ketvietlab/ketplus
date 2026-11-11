#include "KetIcons.h"
#include "KetTokens.h"
#include "agent/AgentWorkspaceView.h"
#include "app/ApplicationAttributes.h"
#include "app/AtlasBuilderTab.h"
#include "app/AtlasConversationPanel.h"
#include "app/MainWindow.h"
#include "atlas/AtlasTab.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/AtlasIndex.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QQuickItem>
#include <QQuickView>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QWebView>
#include <QtTest>

class AtlasWorkspaceTest final : public QObject {
    Q_OBJECT
    static bool writeAtlas(const QString& path, const QString& title) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        const QByteArray data =
            QStringLiteral(
                R"({"version":1,"title":"%1","screens":[],"flows":[{"id":"primary","title":"Primary flow"}]})")
                .arg(title)
                .toUtf8();
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }

  private slots:
    void transcriptAtlasResultOpensExistingBuilder() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(writeAtlas(workspace.filePath("sample.ketatlas/atlas.json"), "Sample Atlas"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        auto* bar = window.findChild<ketplus::ActivityBar*>();
        QVERIFY(bar);
        auto* agentButton = bar->findChild<QAbstractButton*>("agentWorkspaceActivityButton");
        QVERIFY(agentButton);
        agentButton->click();
        auto* chat = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(chat);
        chat->setActivePath(workspace.path());
        chat->setExecutionBranch("main", workspace.path(), 1);
        const auto receipt = chat->inspectAtlasResult("sample.ketatlas/atlas.json");
        QVERIFY(receipt.value("available").toBool());
        QCOMPARE(receipt.value("title").toString(), QString("Sample Atlas"));
        QCOMPARE(receipt.value("screens").toInt(), 0);
        QCOMPARE(receipt.value("flows").toInt(), 1);
        const auto previousCli = qgetenv("KETPLUS_KETATLAS_CLI");
        const auto restoreCli = qScopeGuard([previousCli] {
            if (previousCli.isNull())
                qunsetenv("KETPLUS_KETATLAS_CLI");
            else
                qputenv("KETPLUS_KETATLAS_CLI", previousCli);
        });
        qputenv("KETPLUS_KETATLAS_CLI", "/missing/ketatlas");
        QSignalSpy viewerChanges(chat, &ketplus::AgentWorkspaceView::atlasResultsChanged);
        chat->openAtlasResult("sample.ketatlas/atlas.json", true);
        QVERIFY(!viewerChanges.isEmpty());
        QVERIFY(!chat->inspectAtlasResult("sample.ketatlas/atlas.json")
                     .value("viewerError")
                     .toString()
                     .isEmpty());
        chat->openAtlasResult("sample.ketatlas/atlas.json", false);
        auto* tabs = window.findChild<QTabWidget*>("atlasWorkspaceTabs");
        QVERIFY(tabs);
        auto* builder = qobject_cast<ketplus::AtlasBuilderTab*>(tabs->currentWidget());
        QVERIFY(builder);
        QTRY_COMPARE(
            builder->atlasPath(),
            QFileInfo(workspace.filePath("sample.ketatlas/atlas.json")).canonicalFilePath());
        QVERIFY(builder->chatView()
                    ->inspectAtlasResult("sample.ketatlas/atlas.json")
                    .value("available")
                    .toBool());
    }
    void conversationHeaderOpensAtlasListAndEdit() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(writeAtlas(workspace.filePath("sample.ketatlas/atlas.json"),
                           QStringLiteral("Sample Atlas")));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1440, 900);
        window.show();
        auto* bar = window.findChild<ketplus::ActivityBar*>();
        QVERIFY(bar);
        QVERIFY(!bar->findChild<QAbstractButton*>(QStringLiteral("atlasActivityButton")));
        auto* agentButton =
            bar->findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
        QVERIFY(agentButton);
        agentButton->click();
        auto* workspaceView = window.findChild<ketplus::AgentWorkspaceView*>();
        QVERIFY(workspaceView);
        auto* headerAtlas = workspaceView->quickView()->rootObject()->findChild<QQuickItem*>(
            QStringLiteral("headerAtlasButton"));
        QVERIFY(headerAtlas);
        workspaceView->requestProductNavigation(QStringLiteral("atlas"));
        auto* panel = window.findChild<ketplus::AtlasConversationPanel*>(
            QStringLiteral("conversationAtlasPanel"));
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        const QPoint nativeHeader =
            workspaceView->quickView()->mapToGlobal(headerAtlas->mapToScene(QPointF()).toPoint()) -
            window.mapToGlobal(QPoint());
        qInfo() << "Atlas header Qt position" << nativeHeader << "size" << headerAtlas->width()
                << headerAtlas->height();
        qInfo() << "Agent view origin" << workspaceView->mapTo(&window, QPoint()) << "size"
                << workspaceView->size() << "Quick size" << workspaceView->quickView()->size()
                << "QQuickView scene" << headerAtlas->mapToScene(QPointF());
        auto* panelTitle = panel->findChild<QLabel*>(QStringLiteral("conversationAtlasTitle"));
        auto* panelSearch = panel->findChild<QLineEdit*>(QStringLiteral("conversationAtlasSearch"));
        QVERIFY(panelTitle && panelSearch);
        qInfo() << "Atlas panel title Qt position" << panelTitle->mapTo(&window, QPoint())
                << "search" << panelSearch->mapTo(&window, QPoint());
        if (const auto capture = qEnvironmentVariable("KETPLUS_UI_PARITY_CAPTURE");
            !capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        QTRY_COMPARE(panel->findChildren<QWidget*>(QStringLiteral("conversationAtlasRow")).size(),
                     1);
        const auto priorCli = qgetenv("KETPLUS_KETATLAS_CLI");
        qputenv("KETPLUS_KETATLAS_CLI", "/missing/ketatlas");
        auto* open = panel->findChild<QAbstractButton*>(QStringLiteral("conversationAtlasOpen"));
        QVERIFY(open);
        open->click();
        QTRY_VERIFY(
            panel->findChild<QAbstractButton*>(QStringLiteral("conversationAtlasInstallCli")));
        if (priorCli.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", priorCli);
        QList<QWidget*> nativeBefore;
        for (QWidget* page = workspaceView; page != nullptr && page != &window;
             page = page->parentWidget())
            if (page->internalWinId() != 0)
                nativeBefore << page;
        auto* edit = panel->findChild<QAbstractButton*>(QStringLiteral("conversationAtlasEdit"));
        QVERIFY(edit);
        edit->click();
        auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("atlasWorkspaceTabs"));
        QVERIFY(tabs);
        auto* builder = qobject_cast<ketplus::AtlasBuilderTab*>(tabs->currentWidget());
        QVERIFY(builder);
        QTRY_COMPARE(
            builder->atlasPath(),
            QFileInfo(workspace.filePath("sample.ketatlas/atlas.json")).canonicalFilePath());
        // The builder chat is a native window. Its siblings must stay as they were: the page
        // around the Agent Workspace's QQuickView, made native, covered the conversation and
        // every other page after an Atlas had been opened.
        for (QWidget* page = workspaceView; page != nullptr && page != &window;
             page = page->parentWidget())
            // The builder's own ancestors turn native with it; they cover nothing.
            QVERIFY2(page->internalWinId() == 0 || nativeBefore.contains(page) ||
                         page->isAncestorOf(builder),
                     page->metaObject()->className());
        agentButton->click();
        QTRY_VERIFY(workspaceView->isVisible());
    }

    void mockSidebarGeometryAtDesktopViewport() {
        QTemporaryDir settings;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.resize(1440, 900);
        window.show();
        auto* bar = window.findChild<ketplus::ActivityBar*>();
        QVERIFY(bar);
        const QList<QPair<QString, int>> landmarks = {
            {QStringLiteral("agentWorkspaceActivityButton"), 94},
            {QStringLiteral("explorerActivityButton"), 130},
            {QStringLiteral("sourceControlActivityButton"), 166},
            {QStringLiteral("remoteActivityButton"), 202},
            {QStringLiteral("productMachineSelect"), 251},
        };
        for (const auto& [name, mockY] : landmarks) {
            auto* control = bar->findChild<QWidget*>(name);
            QVERIFY2(control, qPrintable(name));
            const int actualY = control->mapTo(bar, QPoint()).y();
            qInfo().noquote() << name << "mock y=" << mockY << "Qt y=" << actualY
                              << "height=" << control->height();
            QVERIFY2(qAbs(actualY - mockY) < mockY * 0.05,
                     qPrintable(name + QStringLiteral(" exceeds the 5% placement budget")));
        }
        auto* machineSelect = bar->findChild<QComboBox*>(QStringLiteral("productMachineSelect"));
        QVERIFY(machineSelect);
        QVERIFY(bar->styleSheet().contains(QStringLiteral("QComboBox#productMachineSelect")));
        bar->setMachines({QStringLiteral("local"), QStringLiteral("linux-1")},
                         {QStringLiteral("This Mac"), QStringLiteral("Linux lab")},
                         QStringLiteral("linux-1"));
        QCOMPARE(machineSelect->currentData().toString(), QStringLiteral("linux-1"));
        bar->setRemoteMachineSelected(true);
        QVERIFY(bar->findChild<QLabel*>(QStringLiteral("productMachineNotice"))->isVisible());
        QVERIFY(!bar->findChild<QAbstractButton*>(QStringLiteral("productAddRepositoryButton"))
                     ->isEnabled());
    }

    void ownsSeparateTabs() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QVERIFY(
            writeAtlas(workspace.filePath("first.ketatlas/atlas.json"), QStringLiteral("First")));
        QVERIFY(
            writeAtlas(workspace.filePath("second.ketatlas/atlas.json"), QStringLiteral("Second")));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        const auto oldCli = qgetenv("KETPLUS_KETATLAS_CLI");
        qputenv("KETPLUS_KETATLAS_CLI", "/missing/ketatlas");
        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1000, 700);
        window.show();
        auto* bar = window.findChild<ketplus::ActivityBar*>();
        auto* atlasButton = window.findChild<QAbstractButton*>("atlasActivityButton");
        auto* explorerButton = window.findChild<QAbstractButton*>("explorerActivityButton");
        auto* newAtlas = window.findChild<QAction*>("newAtlasTabAction");
        QVERIFY(bar && explorerButton && newAtlas);
        QVERIFY(!atlasButton); // Atlas is reached from the conversation header, not the sidebar.
        newAtlas->trigger();
        auto* tabs = window.findChild<QTabWidget*>("atlasWorkspaceTabs");
        auto* addTab = window.findChild<QAbstractButton*>("atlasNewTabButton");
        QVERIFY(tabs);
        QVERIFY(addTab);
        QVERIFY(!ket::Icons::codeForLucide(u"plus").isEmpty());
        QVERIFY(!ket::Icons::codeForLucide(u"x").isEmpty());
        QCOMPARE(tabs->cornerWidget(Qt::TopRightCorner), addTab);
        QTRY_VERIFY(tabs->rect().contains(QRect(addTab->mapTo(tabs, QPoint()), addTab->size())));
        QVERIFY(tabs->isVisible());
        QVERIFY(!tabs->tabBar()->expanding());
        QCOMPARE(bar->mode(), ketplus::ActivityBar::Mode::Atlas);
        QVERIFY(!window.statusBar()->isVisible());
        QCOMPARE(tabs->count(), 1);
        auto* first = qobject_cast<ketplus::AtlasTab*>(tabs->widget(0));
        QVERIFY(first);
        auto* firstGrid = first->findChild<QListWidget*>("atlasGrid");
        QTRY_COMPARE(firstGrid->count(), 2);
        theme.setMode(ketplus::ThemeManager::Mode::Dark);
        QTRY_VERIFY(first->styleSheet().contains(
            ket::Tokens::color(u"page-bg", ket::Theme::Dark).name(QColor::HexArgb)));
        theme.setMode(ketplus::ThemeManager::Mode::Light);
        QTRY_VERIFY(first->styleSheet().contains(
            ket::Tokens::color(u"page-bg", ket::Theme::Light).name(QColor::HexArgb)));
        auto* createAtlas = first->findChild<QAbstractButton*>("atlasCreateButton");
        QVERIFY(createAtlas);
        createAtlas->click();
        QCOMPARE(tabs->count(), 1);
        auto* builder = qobject_cast<ketplus::AtlasBuilderTab*>(tabs->currentWidget());
        QVERIFY(builder);
        theme.setMode(ketplus::ThemeManager::Mode::Dark);
        QTRY_VERIFY(builder->styleSheet().contains(
            ket::Tokens::color(u"page-bg", ket::Theme::Dark).name(QColor::HexArgb)));
        theme.setMode(ketplus::ThemeManager::Mode::Light);
        QTRY_VERIFY(builder->styleSheet().contains(
            ket::Tokens::color(u"page-bg", ket::Theme::Light).name(QColor::HexArgb)));
        QVERIFY(builder->conversationId().size() > 0);
        QVERIFY(builder->chatView()->compactMode());
        auto* placeholder = builder->findChild<QLabel*>("atlasBuilderPlaceholderTitle");
        auto* explanation = builder->findChild<QLabel*>("atlasBuilderExplanation");
        auto* guidance = builder->findChild<QLabel*>("atlasBuilderGuidance");
        auto* samplePrompt = builder->findChild<QLabel*>("atlasBuilderSamplePrompt");
        QVERIFY(placeholder);
        QVERIFY(explanation);
        QVERIFY(guidance);
        QVERIFY(samplePrompt);
        QCOMPARE(placeholder->text(), QStringLiteral("Your Atlas will appear here."));
        QVERIFY(samplePrompt->text().contains(QStringLiteral("Design onboarding for KetPlus")));
        QVERIFY(samplePrompt->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
        QCoreApplication::processEvents();
        if (const auto screenshot = qEnvironmentVariable("KETPLUS_ATLAS_BUILDER_SCREENSHOT");
            !screenshot.isEmpty()) {
            QVERIFY(window.grab().save(screenshot));
        }
        QVERIFY(placeholder->geometry().bottom() < explanation->geometry().top());
        QVERIFY(explanation->geometry().bottom() < guidance->geometry().top());
        QVERIFY(guidance->geometry().bottom() < samplePrompt->geometry().top());
        QVERIFY(explanation->geometry().top() - placeholder->geometry().bottom() <= 16);
        QVERIFY(guidance->geometry().top() - explanation->geometry().bottom() <= 16);
        QVERIFY(samplePrompt->geometry().top() - guidance->geometry().bottom() <= 16);
        QVERIFY(explanation->height() + 1 >= explanation->heightForWidth(explanation->width()));
        QVERIFY(guidance->height() + 1 >= guidance->heightForWidth(guidance->width()));
        QVERIFY(samplePrompt->height() + 1 >= samplePrompt->heightForWidth(samplePrompt->width()));
        const auto wrappedTextHeight = [](const QLabel* label) {
            return label->fontMetrics()
                .boundingRect(QRect(0, 0, label->width(), 10000), Qt::TextWordWrap, label->text())
                .height();
        };
        QVERIFY(explanation->height() <= wrappedTextHeight(explanation) + 4);
        QVERIFY(guidance->height() <= wrappedTextHeight(guidance) + 4);
        addTab->click();
        QCOMPARE(tabs->count(), 2);
        auto* duplicatePicker = qobject_cast<ketplus::AtlasTab*>(tabs->currentWidget());
        QVERIFY(duplicatePicker);
        auto* duplicateCreate = duplicatePicker->findChild<QAbstractButton*>("atlasCreateButton");
        QVERIFY(duplicateCreate);
        duplicateCreate->click();
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->currentWidget(), builder);
        auto* builderClose = qobject_cast<QAbstractButton*>(
            tabs->tabBar()->tabButton(tabs->indexOf(builder), QTabBar::RightSide));
        QVERIFY(builderClose);
        QCOMPARE(builderClose->objectName(), QStringLiteral("atlasCloseTabButton"));
        builderClose->click();
        QCOMPARE(tabs->count(), 1);
        first = qobject_cast<ketplus::AtlasTab*>(tabs->widget(0));
        QVERIFY(first);
        firstGrid = first->findChild<QListWidget*>("atlasGrid");
        QTRY_COMPARE(firstGrid->count(), 2);
        QTest::mouseClick(firstGrid->viewport(), Qt::LeftButton, Qt::NoModifier,
                          firstGrid->visualItemRect(firstGrid->item(0)).center());
        QTRY_VERIFY(!first->atlasPath().isEmpty());
        addTab->click();
        QCOMPARE(tabs->count(), 2);
        auto* second = qobject_cast<ketplus::AtlasTab*>(tabs->currentWidget());
        QVERIFY(second);
        auto* secondGrid = second->findChild<QListWidget*>("atlasGrid");
        QTRY_COMPARE(secondGrid->count(), 2);
        int secondRow =
            secondGrid->item(0)->data(Qt::UserRole).toString() == first->atlasPath() ? 1 : 0;
        QTest::mouseClick(secondGrid->viewport(), Qt::LeftButton, Qt::NoModifier,
                          secondGrid->visualItemRect(secondGrid->item(secondRow)).center());
        QTRY_VERIFY(!second->atlasPath().isEmpty());
        QVERIFY(second->atlasPath() != first->atlasPath());
        explorerButton->click();
        QVERIFY(!tabs->isVisible());
        newAtlas->trigger();
        QCOMPARE(tabs->count(), 3);
        QVERIFY(tabs->isVisible());
        QCOMPARE(tabs->widget(0), first);
        tabs->setCurrentWidget(second);
        auto* close =
            qobject_cast<QAbstractButton*>(tabs->tabBar()->tabButton(1, QTabBar::RightSide));
        QVERIFY(close);
        QCOMPARE(close->objectName(), QStringLiteral("atlasCloseTabButton"));
        close->click();
        QCOMPARE(tabs->count(), 2);
        QVERIFY(QMetaObject::invokeMethod(tabs, "tabCloseRequested", Q_ARG(int, 0)));
        QCOMPARE(tabs->count(), 1);
        auto* picker = qobject_cast<ketplus::AtlasTab*>(tabs->widget(0));
        QVERIFY(picker && picker->atlasId().isEmpty());
        explorerButton->click();
        newAtlas->trigger();
        QCOMPARE(bar->mode(), ketplus::ActivityBar::Mode::Atlas);
        QCOMPARE(tabs->count(), 2);
        for (auto* host : window.findChildren<QTabWidget*>()) {
            if (host == tabs)
                continue;
            for (int i = 0; i < host->count(); ++i)
                QVERIFY(!qobject_cast<ketplus::AtlasTab*>(host->widget(i)));
        }
        if (const auto screenshot = qEnvironmentVariable("KETPLUS_ATLAS_WORKSPACE_SCREENSHOT");
            !screenshot.isEmpty()) {
            QTest::qWait(200);
            QVERIFY(window.grab().save(screenshot));
        }
        if (oldCli.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", oldCli);
    }

    void builderOpensFirstNewAtlas() {
        QTemporaryDir settings;
        QTemporaryDir workspace;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        const auto oldCli = qgetenv("KETPLUS_KETATLAS_CLI");
        qputenv("KETPLUS_KETATLAS_CLI", "/missing/ketatlas");

        ketplus::ThemeManager theme(*qApp);
        ketplus::MainWindow window(theme);
        window.openFolder(workspace.path());
        window.resize(1000, 700);
        window.show();
        auto* newAtlas = window.findChild<QAction*>("newAtlasTabAction");
        QVERIFY(newAtlas);
        newAtlas->trigger();
        auto* tabs = window.findChild<QTabWidget*>("atlasWorkspaceTabs");
        QVERIFY(tabs);
        auto* picker = qobject_cast<ketplus::AtlasTab*>(tabs->currentWidget());
        QVERIFY(picker);
        auto* create = picker->findChild<QAbstractButton*>("atlasCreateButton");
        QVERIFY(create && create->isVisible());
        create->click();

        auto* builder = qobject_cast<ketplus::AtlasBuilderTab*>(tabs->currentWidget());
        QVERIFY(builder);
        auto* chat = builder->chatView();
        const auto conversation = builder->conversationId();
        auto* frame = builder->findChild<QWidget*>("atlasBuilderChatFrame");
        QVERIFY(frame);
        QCoreApplication::processEvents();
        const auto chatGeometry = frame->geometry();
        auto* atlasIndex = window.findChild<ketplus::AtlasIndex*>();
        QVERIFY(atlasIndex);
        // The initial background scan must finish before publishing this two-file batch.
        // Otherwise it can discover only z-created while a-created is still being written.
        atlasIndex->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!atlasIndex->isIndexing(), 5000);
        QVERIFY(writeAtlas(workspace.filePath("z-created.ketatlas/atlas.json"),
                           QStringLiteral("Z created")));
        QVERIFY(writeAtlas(workspace.filePath("a-created.ketatlas/atlas.json"),
                           QStringLiteral("A created")));
        atlasIndex->scheduleRefresh(0);
        QTRY_VERIFY(builder->hasBuiltAtlas());
        QCOMPARE(
            builder->atlasPath(),
            QFileInfo(workspace.filePath("a-created.ketatlas/atlas.json")).canonicalFilePath());
        auto* collapse = builder->findChild<QAbstractButton*>("atlasBuilderChatCollapse");
        QVERIFY(collapse && collapse->isVisible());
        auto* preview = builder->findChild<ketplus::AtlasTab*>("atlasBuilderPreview");
        QVERIFY(preview);
        QCOMPARE(builder->chatView(), chat);
        QCOMPARE(builder->conversationId(), conversation);
        QCOMPARE(tabs->currentWidget(), builder);
        QCOMPARE(frame->geometry(), chatGeometry);
        QVERIFY(chat->isVisible());
        QVERIFY(!preview->geometry().intersects(frame->geometry()));
        QVERIFY(frame->width() <= 410);
        // The mock names the flow under the Atlas and asks for a change to it in the composer.
        auto* subtitle = builder->findChild<QLabel*>("atlasBuilderToolbarSubtitle");
        QVERIFY(subtitle);
        QCOMPARE(subtitle->text(), QStringLiteral("Primary flow · 0 screens"));
        QCOMPARE(chat->composerHint(), QStringLiteral("Ask for a change to Primary flow…"));
        auto* validated = builder->findChild<QWidget*>("atlasBuilderValidated");
        QVERIFY(validated && validated->isVisible());
        if (const auto capture = qEnvironmentVariable("KETPLUS_ATLAS_BUILDER_CAPTURE");
            !capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        QCOMPARE(builder->promptContext().value("atlasFlowId").toString(),
                 QStringLiteral("primary"));
        QCOMPARE(builder->promptContext().value("atlasConversationId").toString(), conversation);
        QSignalSpy titles(builder, &ketplus::AtlasBuilderTab::titleChanged);
        QVERIFY(writeAtlas(builder->atlasPath(), QStringLiteral("Updated while streaming")));
        QTRY_VERIFY_WITH_TIMEOUT(!titles.isEmpty(), 5000);
        QCOMPARE(titles.last().first().toString(), QStringLiteral("Updated while streaming"));
        QCOMPARE(builder->chatView(), chat);
        QCOMPARE(builder->conversationId(), conversation);
        QCOMPARE(tabs->currentWidget(), builder);
        QVERIFY(chat->isVisible());
        titles.clear();
        QFile html(workspace.filePath("a-created.ketatlas/screen.html"));
        QVERIFY(html.open(QIODevice::WriteOnly));
        html.write("<h1>New screen without a manifest change</h1>");
        html.close();
        QTRY_VERIFY_WITH_TIMEOUT(!titles.isEmpty(), 5000);
        QCOMPARE(builder->chatView(), chat);
        collapse->click();
        QVERIFY(!chat->isVisible());
        QCOMPARE(preview->width(), builder->width());
        auto* aiButton = builder->findChild<QAbstractButton*>("atlasBuilderAiButton");
        QVERIFY(aiButton && aiButton->isVisible());
        aiButton->click();
        QVERIFY(chat->isVisible());
        QCOMPARE(frame->geometry(), chatGeometry);

        QVERIFY(QFile::remove(builder->atlasPath()));
        atlasIndex->scheduleRefresh(0);
        auto* chooseAnother = builder->findChild<QAbstractButton*>("atlasChooseAnotherButton");
        QVERIFY(chooseAnother);
        QTRY_VERIFY_WITH_TIMEOUT(chooseAnother->isVisible(), 5000);
        chooseAnother->click();
        auto* replacementGrid = builder->findChild<QListWidget*>("atlasGrid");
        QVERIFY(replacementGrid);
        QTRY_COMPARE(replacementGrid->count(), 1);
        QTest::mouseClick(replacementGrid->viewport(), Qt::LeftButton, Qt::NoModifier,
                          replacementGrid->visualItemRect(replacementGrid->item(0)).center());
        QTRY_COMPARE(
            builder->atlasPath(),
            QFileInfo(workspace.filePath("z-created.ketatlas/atlas.json")).canonicalFilePath());
        QCOMPARE(builder->conversationId(), conversation);

        if (oldCli.isNull())
            qunsetenv("KETPLUS_KETATLAS_CLI");
        else
            qputenv("KETPLUS_KETATLAS_CLI", oldCli);
    }

    void aiPanelTogglePreservesConversation() {
        QTemporaryDir workspace;
        ketplus::AtlasIndex index;
        index.setRoot(workspace.path());
        ketplus::AtlasBuilderTab builder(&index, workspace.path());
        builder.resize(1100, 750);
        builder.show();
        auto* chat = builder.chatView();
        auto* back = builder.findChild<QAbstractButton*>("atlasBackToConversation");
        QVERIFY(back);
        QVERIFY(!back->isEnabled());
        builder.setConversationId(QStringLiteral("atlas-conversation"));
        QVERIFY(back->isEnabled());
        QSignalSpy backRequested(&builder, &ketplus::AtlasBuilderTab::conversationRequested);
        back->click();
        QCOMPARE(backRequested.count(), 1);
        QCOMPARE(backRequested.first().at(0).toString(), workspace.path());
        QCOMPARE(backRequested.first().at(1).toString(), QStringLiteral("atlas-conversation"));
        auto* collapse = builder.findChild<QAbstractButton*>("atlasBuilderChatCollapse");
        auto* ai = builder.findChild<QAbstractButton*>("atlasBuilderAiButton");
        auto* prompt = chat->quickView()->rootObject()->findChild<QObject*>("agentPrompt");
        QVERIFY(collapse && ai && prompt);
        QVERIFY(chat->isVisible());
        QVERIFY(ai->isChecked());
        prompt->setProperty("text", "Keep my draft");
        chat->setSessionState("Atlas", "Working", true);
        QSignalSpy stopped(chat, &ketplus::AgentWorkspaceView::stopRequested);
        collapse->click();
        QVERIFY(!chat->isVisible());
        QVERIFY(ai->isVisible() && !ai->isChecked());
        QVERIFY(ai->toolTip().contains("AI is working"));
        chat->appendMessage({{"kind", "tool"},
                             {"subtype", "question"},
                             {"running", true},
                             {"questions", QVariantList{QVariantMap{{"id", "choice"},
                                                                    {"question", "Continue?"}}}}});
        QVERIFY(ai->text().contains("Needs answer"));
        QVERIFY(!chat->isVisible());
        ai->click();
        QVERIFY(chat->isVisible());
        QCOMPARE(builder.chatView(), chat);
        QCOMPARE(prompt->property("text").toString(), QStringLiteral("Keep my draft"));
        QVERIFY(chat->sessionRunning());
        QCOMPARE(stopped.count(), 0);
        chat->updateMessage(0, {{"kind", "tool"}, {"subtype", "question"}, {"running", false}});
        QCOMPARE(ai->text(), QStringLiteral("Hide AI Chat"));
        chat->setSessionState("Atlas", "Ready", false);
        QVERIFY(ai->toolTip().contains("AI is idle"));
    }

    void nativeBuilderRetainsChatDuringLiveReload() {
        if (!qEnvironmentVariableIsSet("KETPLUS_NATIVE_ATLAS_TEST"))
            QSKIP("Enable native WebView test explicitly");
        QTemporaryDir workspace;
        ketplus::AtlasIndex index;
        index.setRoot(workspace.path());
        ketplus::AtlasBuilderTab builder(&index, workspace.path());
        builder.setConversationId(QStringLiteral("live-builder"));
        builder.resize(1200, 750);
        builder.show();
        auto* chat = builder.chatView();
        chat->resetMessages({QVariantMap{{"kind", "assistant"}, {"body", "Building screens"}}},
                            "live-builder");
        chat->setSessionState("Atlas", "Working", true);
        auto* preview = builder.findChild<ketplus::AtlasTab*>("atlasBuilderPreview");
        QVERIFY(preview);
        QSignalSpy loaded(preview, &ketplus::AtlasTab::loaded);
        const auto write = [](const QString& path, const QByteArray& content) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
        };
        const auto htmlPath = workspace.filePath("live.ketatlas/screen.html");
        QVERIFY(write(htmlPath, "<h1>First version</h1>"));
        QVERIFY(write(
            workspace.filePath("live.ketatlas/atlas.json"),
            R"({"version":1,"title":"Live","screens":[{"id":"home","title":"Home","url":"./screen.html"}],"flows":[{"id":"main","title":"Main","nodes":[{"id":"entry","screen":"home"}],"edges":[]}]})"));
        index.scheduleRefresh(0);
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 30000);
        QWebView* browser = nullptr;
        for (auto* window : QGuiApplication::allWindows())
            if (auto* web = qobject_cast<QWebView*>(window))
                browser = web;
        QVERIFY(browser);
        loaded.clear();
        QVERIFY(write(htmlPath, "<h1>Updated by AI</h1>"));
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 10000);
        QVERIFY(QGuiApplication::allWindows().contains(browser));
        QCOMPARE(builder.chatView(), chat);
        QCOMPARE(builder.conversationId(), QStringLiteral("live-builder"));
        QVERIFY(chat->isVisible());
        auto* frame = builder.findChild<QWidget*>("atlasBuilderChatFrame");
        QVERIFY(frame && !preview->geometry().intersects(frame->geometry()));
        QVERIFY(frame->width() <= 410);
        if (const auto path = qEnvironmentVariable("KETPLUS_ATLAS_BUILDER_SCREENSHOT");
            !path.isEmpty())
            QVERIFY(builder.screen()->grabWindow(builder.winId()).save(path));
    }

    // #155 follow-up: an Atlas opened after choosing a zoom starts at it, not at 100%.
    void atlasTabsOpenAtTheCurrentZoom() {
        ketplus::ThemeManager theme(*qApp);
        const auto restore = qScopeGuard([&theme] { theme.setInterfaceScale(14, 100); });
        theme.setInterfaceScale(14, 125);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ketplus::AtlasIndex index;
        ketplus::AtlasTab atlas(&index);
        QCOMPARE(atlas.property("_ketChromeZoom").toInt(), 125);
        ketplus::AtlasBuilderTab builder(&index, root.path());
        QCOMPARE(builder.property("_ketChromeZoom").toInt(), 125);
    }
};

int main(int argc, char** argv) {
    ketplus::applyApplicationAttributes();
    QApplication app(argc, argv);
    AtlasWorkspaceTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "AtlasWorkspaceTest.moc"
