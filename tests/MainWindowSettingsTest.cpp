#include "KetSegmentedControl.h"
#include "KetThemeCard.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentConversationWidth.h"
#include "app/AppearanceSettings.h"
#include "app/ArchivedSettingsPage.h"
#include "app/MainWindow.h"
#include "app/SettingsBrowserPage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsProvidersPage.h"
#include "app/SettingsWorkspacePage.h"
#include "app/WorktreesWorkspacePage.h"
#include "editor/EditorWidget.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalView.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include "update/AppUpdater.h"
#include "workspace/ActivityBar.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>

#include <QAbstractButton>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include <atomic>

namespace {

QAction* actionWithText(QObject& parent, const QString& text) {
    for (auto* action : parent.findChildren<QAction*>()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

class FakeAppUpdater final : public ketplus::AppUpdater {
  public:
    explicit FakeAppUpdater(const bool configured) : configured_(configured) {}

    [[nodiscard]] bool isConfigured() const override { return configured_; }
    [[nodiscard]] bool canCheckForUpdates() const override { return configured_ && canCheck_; }
    void checkForUpdates() override { ++checks_; }
    [[nodiscard]] bool automaticallyChecksForUpdates() const override { return automaticChecks_; }
    void setAutomaticallyChecksForUpdates(const bool enabled) override {
        automaticChecks_ = enabled;
        emit stateChanged();
    }
    [[nodiscard]] bool automaticallyDownloadsUpdates() const override {
        return automaticDownloads_;
    }
    void setAutomaticallyDownloadsUpdates(const bool enabled) override {
        automaticDownloads_ = enabled;
        emit stateChanged();
    }
    [[nodiscard]] QDateTime lastUpdateCheck() const override { return lastCheck_; }

    void setCanCheck(const bool canCheck) {
        canCheck_ = canCheck;
        emit stateChanged();
    }
    void remind(const QString& version) { setPendingVersion(version); }

    int checks_{0};
    QDateTime lastCheck_;

  private:
    bool configured_;
    bool canCheck_{true};
    bool automaticChecks_{true};
    bool automaticDownloads_{false};
};

// Installs a fake process-wide updater for one test and restores the default afterwards.
class ScopedAppUpdater final {
  public:
    explicit ScopedAppUpdater(ketplus::AppUpdater& updater) {
        ketplus::AppUpdater::setInstanceForTesting(&updater);
    }
    ~ScopedAppUpdater() { ketplus::AppUpdater::setInstanceForTesting(nullptr); }
    ScopedAppUpdater(const ScopedAppUpdater&) = delete;
    ScopedAppUpdater& operator=(const ScopedAppUpdater&) = delete;
};

} // namespace

class MainWindowSettingsTest final : public QObject {
    Q_OBJECT

  private slots:
    void appliesSavedAppearanceToPublicAndPrivateSurfaces();
    void archivedPageStopsRunningTurnAndRestoresInPlace();
    void settingsAndWorktreesUseWorkspacePages();
    void browserPageDefaultsOnAndPersistsChoices();
    void providersPageShowsRuntimeStatus();
    void conversationWidthFollowsOnlyTheAgentWorkspace();
    void updatesPageAndReminderDriveTheUpdater();
    void unconfiguredBuildExplainsUpdatesInsteadOfChecking();
};

void MainWindowSettingsTest::providersPageShowsRuntimeStatus() {
    const QJsonArray providers{
        QJsonObject{{"provider", "codex"},
                    {"name", "Codex CLI"},
                    {"status", "outdated"},
                    {"executable", "/test/bin/codex"},
                    {"version", "0.120.0"},
                    {"minimum", "0.142.1"},
                    {"newest_tested", "0.154.0"},
                    {"update_command", "npm install -g @openai/codex@latest"},
                    {"message", "Codex CLI 0.120.0 is older than the oldest version KetPlus "
                                "supports (0.142.1)."}},
        QJsonObject{{"provider", "claude"},
                    {"name", "Claude Agent SDK"},
                    {"status", "bundled"},
                    {"version", "0.3.1"},
                    {"components", QJsonObject{{"sdk", "0.3.1"}, {"claude_code", "2.1.1"}}}},
        QJsonObject{{"provider", "cursor"}, {"name", "Cursor CLI"}, {"status", "missing"}}};
    int fetches = 0;
    ketplus::SettingsProvidersPage page([&fetches, providers](auto reply) {
        ++fetches;
        reply(providers, {});
    });
    QSignalSpy refreshed(&page, &ketplus::SettingsProvidersPage::refreshed);
    QTRY_COMPARE(refreshed.count(), 1);
    QCOMPARE(page.providers().size(), 3);
    auto* codex = page.findChild<QWidget*>(QStringLiteral("providerRuntime-codex"));
    QVERIFY(codex != nullptr);
    QCOMPARE(codex->findChild<QLabel*>(QStringLiteral("providerRuntimeStatus"))->text(),
             QStringLiteral("Update required"));
    QCOMPARE(codex->findChild<QLabel*>(QStringLiteral("providerRuntimeUpdate"))->text(),
             QStringLiteral("npm install -g @openai/codex@latest"));
    auto* claude = page.findChild<QWidget*>(QStringLiteral("providerRuntime-claude"));
    QVERIFY(claude != nullptr);
    QVERIFY(claude->findChild<QLabel*>(QStringLiteral("providerRuntimeVersion"))
                ->text()
                .contains(QStringLiteral("Claude Code 2.1.1")));
    QVERIFY(claude->findChild<QLabel*>(QStringLiteral("providerRuntimeUpdate")) == nullptr);
    auto* cursor = page.findChild<QWidget*>(QStringLiteral("providerRuntime-cursor"));
    QVERIFY(cursor != nullptr);
    QVERIFY(cursor->findChild<QLabel*>(QStringLiteral("providerRuntimeVersion")) == nullptr);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("providersStatus"))->text(),
             QStringLiteral("1 provider(s) need attention."));

    page.findChild<QAbstractButton*>(QStringLiteral("providersRecheck"))->click();
    QTRY_COMPARE(refreshed.count(), 2);
    QCOMPARE(fetches, 2);
    // A refresh rebuilds the rows instead of appending to them.
    QCOMPARE(page.findChildren<QWidget*>(QStringLiteral("providerRuntime-codex")).size(), 1);

    ketplus::SettingsProvidersPage failing(
        [](auto reply) { reply({}, QStringLiteral("daemon offline")); });
    QTRY_VERIFY(failing.findChild<QLabel*>(QStringLiteral("providersStatus"))
                    ->text()
                    .contains(QStringLiteral("daemon offline")));
}

void MainWindowSettingsTest::browserPageDefaultsOnAndPersistsChoices() {
    QTemporaryDir root;
    const QString app = root.filePath(QStringLiteral("apps/browser"));
    QVERIFY(QDir().mkpath(app + QStringLiteral("/src")));
    QFile script(app + QStringLiteral("/src/mcp.mjs"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.close();
    const QString settingsPath = root.filePath(QStringLiteral("browser-tools.json"));
    // Shared with worker threads, so the probe state is atomic and outlives the page.
    static std::atomic_bool chromium{false};
    static std::atomic_int slowMilliseconds{0};
    static std::atomic_int nodeVersionMajor{22};
    chromium = false;
    ketplus::browser_tools::Probes probes;
    probes.nodeCandidates = [] { return QStringList{QStringLiteral("/fake/node")}; };
    probes.nodeVersion = [](const QString&) {
        const int delay = slowMilliseconds.exchange(0);
        if (delay > 0)
            QThread::msleep(static_cast<unsigned long>(delay));
        return QStringLiteral("v%1.16.0").arg(nodeVersionMajor.load());
    };
    probes.chromiumInstalled = [](const QString&, const QString&) { return chromium.load(); };

    ketplus::SettingsBrowserPage page(settingsPath, root.path(), probes);
    auto* enabled = page.findChild<QAbstractButton*>(QStringLiteral("browserToolsEnabled"));
    auto* all = page.findChild<QRadioButton*>(QStringLiteral("browserToolsAllOrigins"));
    auto* only = page.findChild<QRadioButton*>(QStringLiteral("browserToolsOnlyOrigins"));
    auto* origins = page.findChild<QLineEdit*>(QStringLiteral("browserToolsOrigins"));
    auto* status = page.findChild<QLabel*>(QStringLiteral("browserToolsStatus"));
    QVERIFY(enabled && all && only && origins && status);
    // Defaults: on, all websites, the origin list unused.
    QVERIFY(enabled->isChecked());
    QVERIFY(enabled->isEnabled());
    QVERIFY(all->isChecked());
    QVERIFY(!origins->isEnabled());
    QTRY_VERIFY(status->text().contains(QStringLiteral("npm ci")));
    QVERIFY(page.runtimeSource().isEmpty());
    QVERIFY(enabled->isEnabled()); // never gated on the runtime

    QVERIFY(QDir().mkpath(app + QStringLiteral("/node_modules/@modelcontextprotocol/server")));
    QVERIFY(QDir().mkpath(app + QStringLiteral("/node_modules/playwright-core")));
    page.refresh();
    QTRY_VERIFY(status->text().contains(QStringLiteral("install:browser")));
    chromium = true;
    page.refresh(); // cache hit shows the stale result first, then revalidates
    QTRY_VERIFY(!page.checking());
    QVERIFY2(status->text().startsWith(QStringLiteral("Ready")), qPrintable(status->text()));
    QCOMPARE(page.runtimeSource(), QStringLiteral("source-checkout"));
    // A cache hit derives the source at once (file checks), matching the full check.
    page.refresh();
    QVERIFY(page.checking());
    QCOMPARE(page.runtimeSource(), QStringLiteral("source-checkout"));
    QVERIFY(status->text().startsWith(QStringLiteral("Ready")));
    QTRY_VERIFY(!page.checking());
    QCOMPARE(ketplus::browser_tools::loadSettings(settingsPath).nodePath,
             QStringLiteral("/fake/node"));

    // Slow probes never block the GUI thread.
    QFile lock(app + QStringLiteral("/package-lock.json"));
    QVERIFY(lock.open(QIODevice::WriteOnly));
    lock.write("{\"lockfileVersion\":3}");
    lock.close();
    slowMilliseconds = 1500;
    QElapsedTimer timer;
    timer.start();
    page.refresh();
    QVERIFY2(timer.elapsed() < 50, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(status->text().contains(QStringLiteral("Checking")));
    // A newer refresh supersedes the slow one; the slow (stale) result must be dropped.
    QTest::qWait(100); // the slow worker has consumed its delay
    nodeVersionMajor = 20;
    QVERIFY(lock.open(QIODevice::WriteOnly));
    lock.write("{\"lockfileVersion\":3,\"v\":2}");
    lock.close();
    page.refresh();
    QTRY_VERIFY(status->text().contains(QStringLiteral("Node.js 22")));
    nodeVersionMajor = 22;
    QTest::qWait(1800); // let the slow worker finish with a (stale) ready result
    QVERIFY2(status->text().contains(QStringLiteral("Node.js 22")), qPrintable(status->text()));
    QTRY_VERIFY(!page.checking());
    QVERIFY(lock.open(QIODevice::WriteOnly));
    lock.write("{\"lockfileVersion\":3,\"v\":3}");
    lock.close();
    page.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!page.checking(), 5000);
    QVERIFY2(status->text().startsWith(QStringLiteral("Ready")), qPrintable(status->text()));

    // "Only these origins" starts from loopback and saves a normalized list.
    only->setChecked(true);
    QVERIFY(origins->isEnabled());
    QCOMPARE(origins->text(), QStringLiteral("loopback"));
    QCOMPARE(ketplus::browser_tools::loadSettings(settingsPath).allowedOrigins,
             QStringLiteral("loopback"));
    origins->setText(QStringLiteral("loopback, http://localhost:5173/"));
    emit origins->editingFinished();
    QCOMPARE(ketplus::browser_tools::loadSettings(settingsPath).allowedOrigins,
             QStringLiteral("loopback,http://localhost:5173"));
    origins->setText(QStringLiteral("ftp://example.com"));
    emit origins->editingFinished();
    QVERIFY(status->text().contains(QStringLiteral("not saved")));
    QCOMPARE(ketplus::browser_tools::loadSettings(settingsPath).allowedOrigins,
             QStringLiteral("loopback,http://localhost:5173"));

    // A refresh never rewrites user fields: another writer's values survive.
    auto external = ketplus::browser_tools::loadSettings(settingsPath);
    external.allowedOrigins = QStringLiteral("http://[::1]:3000");
    QVERIFY(ketplus::browser_tools::saveSettings(external, settingsPath));
    page.refresh();
    QTRY_VERIFY(!page.checking());
    QCOMPARE(ketplus::browser_tools::loadSettings(settingsPath).allowedOrigins,
             QStringLiteral("http://[::1]:3000"));

    all->setChecked(true);
    QVERIFY(!origins->isEnabled());
    QCOMPARE(ketplus::browser_tools::loadSettings(settingsPath).allowedOrigins,
             QStringLiteral("*"));

    // An explicit "off" sticks across reloads (schema version 2), even with a broken runtime.
    chromium = false;
    enabled->setChecked(false);
    QVERIFY(!ketplus::browser_tools::loadSettings(settingsPath).enabled);
    ketplus::SettingsBrowserPage reopened(settingsPath, root.path(), probes);
    QVERIFY(
        !reopened.findChild<QAbstractButton*>(QStringLiteral("browserToolsEnabled"))->isChecked());
    enabled->setChecked(true);
    QVERIFY(ketplus::browser_tools::loadSettings(settingsPath).enabled);

    // A hand-edited list containing "*" launches with all websites, so the page says so.
    auto mixed = ketplus::browser_tools::loadSettings(settingsPath);
    mixed.allowedOrigins = QStringLiteral("loopback, *");
    QVERIFY(ketplus::browser_tools::saveSettings(mixed, settingsPath));
    ketplus::SettingsBrowserPage mixedPage(settingsPath, root.path(), probes);
    QVERIFY(
        mixedPage.findChild<QRadioButton*>(QStringLiteral("browserToolsAllOrigins"))->isChecked());
    QVERIFY(!mixedPage.findChild<QLineEdit*>(QStringLiteral("browserToolsOrigins"))->isEnabled());

    // A damaged file loads off, says so, and is not rewritten by a refresh.
    const QString damagedPath = root.filePath(QStringLiteral("damaged/browser-tools.json"));
    QVERIFY(QDir().mkpath(QFileInfo(damagedPath).absolutePath()));
    const QByteArray garbage("{not json");
    {
        QFile damaged(damagedPath);
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write(garbage);
    }
    ketplus::SettingsBrowserPage damagedPage(damagedPath, root.path(), probes);
    QVERIFY(!damagedPage.findChild<QAbstractButton*>(QStringLiteral("browserToolsEnabled"))
                 ->isChecked());
    auto* damagedStatus = damagedPage.findChild<QLabel*>(QStringLiteral("browserToolsStatus"));
    QVERIFY2(damagedStatus->text().contains(QStringLiteral("could not be read")),
             qPrintable(damagedStatus->text()));
    QSignalSpy damagedChecked(&damagedPage, &ketplus::SettingsBrowserPage::prerequisitesChecked);
    QTRY_COMPARE_WITH_TIMEOUT(damagedChecked.count(), 1, 5000); // the async check keeps the error
    QVERIFY2(damagedStatus->text().contains(QStringLiteral("could not be read")),
             qPrintable(damagedStatus->text()));
    QFile damaged(damagedPath);
    QVERIFY(damaged.open(QIODevice::ReadOnly));
    QCOMPARE(damaged.readAll(), garbage);
}

void MainWindowSettingsTest::archivedPageStopsRunningTurnAndRestoresInPlace() {
    QVariantList archived{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("conversation-1")},
                    {QStringLiteral("path"), QStringLiteral("/workspace")},
                    {QStringLiteral("title"), QStringLiteral("Running conversation")},
                    {QStringLiteral("kind"), QStringLiteral("conversation")},
                    {QStringLiteral("running"), true}},
        QVariantMap{{QStringLiteral("id"), QString{}},
                    {QStringLiteral("path"), QStringLiteral("/workspace/tree")},
                    {QStringLiteral("title"), QStringLiteral("tree")},
                    {QStringLiteral("kind"), QStringLiteral("worktree")}},
    };
    QString stopped;
    QString restored;
    ketplus::ArchivedSettingsPage page(
        archived,
        [&restored](const QString&, const QString& id) {
            restored = id;
            return true;
        },
        [&stopped](const QString&, const QString& id) {
            stopped = id;
            return true;
        });
    auto* list = page.findChild<QListWidget*>(QStringLiteral("archivedConversationList"));
    auto* stop = page.findChild<QAbstractButton*>(QStringLiteral("stopArchivedTurnButton"));
    auto* restore = page.findChild<QAbstractButton*>(QStringLiteral("restoreArchivedItemButton"));
    QVERIFY(list != nullptr);
    QVERIFY(stop != nullptr);
    QVERIFY(restore != nullptr);
    QCOMPARE(list->count(), 2);

    list->setCurrentRow(0);
    QVERIFY(stop->isEnabled());
    stop->click();
    QCOMPARE(stopped, QStringLiteral("conversation-1"));
    QVERIFY(!stop->isEnabled());

    restore->click();
    QCOMPARE(restored, QStringLiteral("conversation-1"));
    QCOMPARE(list->count(), 1);
}

void MainWindowSettingsTest::appliesSavedAppearanceToPublicAndPrivateSurfaces() {
    QTemporaryDir settingsDirectory;
    QTemporaryDir workspace;
    QVERIFY(settingsDirectory.isValid());
    QVERIFY(workspace.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    const QString markdownPath = workspace.filePath(QStringLiteral("README.md"));
    QFile markdown(markdownPath);
    QVERIFY(markdown.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(markdown.write("# Settings\n\nPrivate shell integration.\n") > 0);
    markdown.close();

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    window.openFile(markdownPath);

    auto* previewAction = actionWithText(window, QStringLiteral("Markdown Preview"));
    auto* terminalAction = actionWithText(window, QStringLiteral("Terminal"));
    auto* settingsAction = actionWithText(window, QStringLiteral("Settings…"));
    QVERIFY(previewAction != nullptr);
    QVERIFY(terminalAction != nullptr);
    QVERIFY(settingsAction != nullptr);
    previewAction->setChecked(true);
    terminalAction->setChecked(true);
    terminalAction->setChecked(false);

    auto* agentButton =
        window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
    QVERIFY(agentButton != nullptr);
    agentButton->click();
    auto* agentWorkspace = window.findChild<ketplus::AgentWorkspaceView*>();
    QVERIFY(agentWorkspace != nullptr);
    agentWorkspace->requestTerminal();
    QTRY_COMPARE(window.findChildren<ketplus::TerminalView*>().size(), 2);

    bool saved = false;
    QTimer::singleShot(0, &window, [&window, &saved] {
        auto* dialog = window.findChild<ketplus::SettingsDialog*>();
        if (dialog == nullptr) {
            return;
        }
        window.findChild<ket::SegmentedControl*>(QStringLiteral("interfaceZoom"))
            ->setCurrentIndex(2);
        window.findChild<QSpinBox*>(QStringLiteral("editorFontSize"))->setValue(17);
        window.findChild<QSpinBox*>(QStringLiteral("editorLineHeight"))->setValue(29);
        window.findChild<QSpinBox*>(QStringLiteral("terminalFontSize"))->setValue(18);
        window.findChild<QSpinBox*>(QStringLiteral("terminalLineHeight"))->setValue(30);
        window.findChild<QSpinBox*>(QStringLiteral("previewFontSize"))->setValue(19);
        window.findChild<QSpinBox*>(QStringLiteral("previewLineHeight"))->setValue(31);
        window.findChild<QAbstractButton*>(QStringLiteral("followSystemTheme"))->setChecked(false);
        auto* themes = dialog->findChild<QListWidget*>(QStringLiteral("installedThemeList"));
        for (int index = 0; index < themes->count(); ++index) {
            if (themes->item(index)->data(Qt::UserRole).toInt() ==
                static_cast<int>(ketplus::ThemeManager::Mode::Dark)) {
                themes->setCurrentRow(index);
                break;
            }
        }
        for (auto* button : window.findChildren<QAbstractButton*>()) {
            if (button->property("kvRole") == QStringLiteral("primaryAction")) {
                saved = true;
                button->click();
                return;
            }
        }
    });
    settingsAction->trigger();
    QTRY_VERIFY(saved);
    QTRY_VERIFY(window.findChild<ketplus::SettingsWorkspacePage*>() != nullptr);
    auto* embeddedSettings = window.findChild<ketplus::SettingsDialog*>();
    QVERIFY(embeddedSettings != nullptr);
    QVERIFY(!embeddedSettings->isWindow());
    QVERIFY(!embeddedSettings->isModal());

    const auto appearance = ketplus::AppearanceSettings::load();
    QCOMPARE(appearance.interfaceFontSizePixels, 16);
    QCOMPARE(appearance.editor.fontSizePixels, 17);
    QCOMPARE(appearance.editor.lineHeightPixels, 29);
    QCOMPARE(appearance.terminal, (ketplus::TextTypographySettings{18, 30}));
    QCOMPARE(appearance.preview, (ketplus::TextTypographySettings{19, 31}));
    QCOMPARE(theme.interfaceFontSizePixels(), 16);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
    QVERIFY(actionWithText(window, QStringLiteral("Dark"))->isChecked());
    QVERIFY(!actionWithText(window, QStringLiteral("System"))->isChecked());

    const auto editors = window.findChildren<ketplus::EditorWidget*>();
    QVERIFY(!editors.isEmpty());
    for (const auto* editor : editors) {
        QCOMPARE(editor->editorSettings(), appearance.editor);
    }
    for (const auto* terminal : window.findChildren<ketplus::TerminalView*>()) {
        QCOMPARE(terminal->font().pixelSize(), 18);
    }

    auto* preview = window.findChild<ketplus::MarkdownPreviewPane*>();
    QVERIFY(preview != nullptr);
    auto* browser = preview->findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
    QVERIFY(browser != nullptr);
    QTRY_VERIFY(
        browser->document()->defaultStyleSheet().contains(QStringLiteral("font-size: 19px")));
}

void MainWindowSettingsTest::settingsAndWorktreesUseWorkspacePages() {
    QTemporaryDir settingsDirectory;
    QTemporaryDir workspace;
    QVERIFY(settingsDirectory.isValid());
    QVERIFY(workspace.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    window.openFolder(workspace.path());

    auto* activity = window.findChild<ketplus::ActivityBar*>();
    QVERIFY(activity != nullptr);
    auto* manageWorktrees = window.findChild<QAction*>(QStringLiteral("manageWorktreesAction"));
    QVERIFY(manageWorktrees != nullptr);
    manageWorktrees->trigger();
    ketplus::WorktreesWorkspacePage* worktreesPage = nullptr;
    QTRY_VERIFY((worktreesPage = window.findChild<ketplus::WorktreesWorkspacePage*>()) != nullptr &&
                worktreesPage->isVisible());
    QVERIFY(!window.statusBar()->isVisible());
    QCOMPARE(activity->mode(), ketplus::ActivityBar::Mode::AgentWorkspace);

    auto* settingsAction = window.findChild<QAction*>(QStringLiteral("productSettingsAction"));
    QVERIFY(settingsAction != nullptr);
    settingsAction->trigger();
    ketplus::SettingsWorkspacePage* settingsPage = nullptr;
    QTRY_VERIFY((settingsPage = window.findChild<ketplus::SettingsWorkspacePage*>()) != nullptr &&
                settingsPage->isVisible());
    QTRY_VERIFY(worktreesPage->isHidden());
    QTRY_VERIFY(settingsPage->width() >= 320);
    QTRY_VERIFY(settingsPage->height() >= 240);
    QCOMPARE(activity->mode(), ketplus::ActivityBar::Mode::Settings);
    QVERIFY(!window.statusBar()->isVisible());
    auto* settingsNavigation =
        settingsPage->findChild<QListWidget*>(QStringLiteral("settingsWorkspaceNavigation"));
    QVERIFY(settingsNavigation != nullptr);
    QCOMPARE(settingsNavigation->count(), 10);
    QCOMPARE(settingsNavigation->item(8)->data(Qt::UserRole).toString(), QStringLiteral("storage"));
    auto* storageShortcut =
        activity->findChild<QAbstractButton*>(QStringLiteral("sidebarStorageSettings"));
    QVERIFY(storageShortcut != nullptr);
    storageShortcut->click();
    QTRY_VERIFY(
        settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspaceStorage"))->isVisible());
    QCOMPARE(settingsNavigation->item(9)->data(Qt::UserRole).toString(), QStringLiteral("updates"));
    QCOMPARE(settingsNavigation->item(4)->data(Qt::UserRole).toString(), QStringLiteral("browser"));
    QCOMPARE(settingsNavigation->item(4)->text(), QStringLiteral("Browser"));
    QCOMPARE(settingsNavigation->item(5)->data(Qt::UserRole).toString(),
             QStringLiteral("computer"));
    QCOMPARE(settingsNavigation->item(5)->text(), QStringLiteral("Computer use"));
    QCOMPARE(settingsNavigation->item(6)->data(Qt::UserRole).toString(),
             QStringLiteral("providers"));
    auto* computerPage =
        settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspaceComputer"));
    QVERIFY(computerPage != nullptr);
    settingsNavigation->setCurrentRow(5);
    QTRY_VERIFY(computerPage->isVisible());
    auto* providersPage =
        settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspaceProviders"));
    QVERIFY(providersPage != nullptr);
    settingsNavigation->setCurrentRow(6);
    QTRY_VERIFY(providersPage->isVisible());
    QVERIFY(!computerPage->isVisible());
    settingsNavigation->setCurrentRow(0);
    QTRY_VERIFY(!providersPage->isVisible());
    QCOMPARE(settingsNavigation->currentItem()->data(Qt::UserRole).toString(),
             QStringLiteral("appearance"));
    auto* lightTheme =
        settingsPage->findChild<ket::ThemeCard*>(QStringLiteral("settingsLightThemeCard"));
    auto* darkTheme =
        settingsPage->findChild<ket::ThemeCard*>(QStringLiteral("settingsDarkThemeCard"));
    auto* settingsTitle =
        settingsPage->findChild<QLabel*>(QStringLiteral("settingsWorkspaceTitle"));
    auto* sectionTitle =
        settingsPage->findChild<QLabel*>(QStringLiteral("settingsWorkspaceSectionTitle"));
    QVERIFY(lightTheme != nullptr);
    QVERIFY(darkTheme != nullptr);
    QVERIFY(settingsTitle != nullptr);
    QVERIFY(sectionTitle != nullptr);
    QCOMPARE(settingsTitle->font().pixelSize(), 30);
    QCOMPARE(sectionTitle->font().pixelSize(), 22);
    QVERIFY(settingsPage->findChild<QFrame*>(QStringLiteral("settingsWorkspacePreview")) !=
            nullptr);
    QVERIFY(!window.findChild<QLineEdit*>(QStringLiteral("themeSearch"))->isVisible());
    darkTheme->click();
    QTRY_VERIFY(theme.palette().dark);
    lightTheme->click();
    QTRY_VERIFY(!theme.palette().dark);
    settingsNavigation->setCurrentRow(1);
    auto* zoom = window.findChild<ket::SegmentedControl*>(QStringLiteral("interfaceZoom"));
    QVERIFY(zoom != nullptr);
    QTRY_VERIFY(zoom->isVisible());
    QCOMPARE(zoom->options(), QStringList({"Small", "Medium", "Large"}));
    QCOMPARE(zoom->currentIndex(), 1);
    // Opening a saved non-preset size must not silently change it.
    auto* numericZoom = window.findChild<QSpinBox*>(QStringLiteral("interfaceFontSize"));
    numericZoom->setValue(15);
    QCOMPARE(zoom->currentIndex(), 3);
    QCOMPARE(zoom->options().last(), QStringLiteral("Custom (15)"));
    QCOMPARE(numericZoom->value(), 15);
    zoom->setCurrentIndex(1);
    QCOMPARE(numericZoom->value(), 14);
    QVERIFY(window.findChild<QSpinBox*>(QStringLiteral("interfaceFontSize"))->isHidden());
    QVERIFY(settingsPage->findChild<QComboBox*>(QStringLiteral("settingsInterfaceFont")) !=
            nullptr);
}

void MainWindowSettingsTest::conversationWidthFollowsOnlyTheAgentWorkspace() {
    QTemporaryDir settingsDirectory;
    QTemporaryDir workspace;
    QVERIFY(settingsDirectory.isValid());
    QVERIFY(workspace.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    window.openFolder(workspace.path());
    window.findChild<QAction*>(QStringLiteral("manageWorktreesAction"))->trigger();
    ketplus::AgentWorkspaceView* agent = nullptr;
    QTRY_VERIFY((agent = window.findChild<ketplus::AgentWorkspaceView*>()) != nullptr);
    QCOMPARE(agent->conversationWidth(), ketplus::AgentWorkspaceView::defaultConversationWidth);
    // Stands in for Atlas's embedded chat, which never reads the preference.
    ketplus::AgentWorkspaceView atlasChat;

    ketplus::SettingsWorkspacePage* settingsPage = nullptr;
    const auto openTypography = [&window, &settingsPage] {
        if (window.findChild<ketplus::SettingsWorkspacePage*>() == nullptr)
            window.findChild<QAction*>(QStringLiteral("productSettingsAction"))->trigger();
        QTRY_VERIFY((settingsPage = window.findChild<ketplus::SettingsWorkspacePage*>()) !=
                        nullptr &&
                    settingsPage->isVisible());
        settingsPage->findChild<QListWidget*>(QStringLiteral("settingsWorkspaceNavigation"))
            ->setCurrentRow(1);
    };
    const auto widthControl = [&settingsPage] {
        return settingsPage->findChild<ket::SegmentedControl*>(
            QStringLiteral("agentConversationWidth"));
    };
    const auto widthIndex = [](int pixels) {
        const auto choices = ketplus::AgentConversationWidth::choices();
        for (int i = 0; i < choices.size(); ++i)
            if (choices[i].pixels == pixels)
                return i;
        return -1;
    };
    const auto selectWidth = [&](int pixels) {
        auto* control = widthControl();
        QTest::mouseClick(control, Qt::LeftButton, Qt::NoModifier,
                          control->segmentRect(widthIndex(pixels)).center());
    };
    const auto saveButton = [&settingsPage]() -> QAbstractButton* {
        for (auto* button : settingsPage->findChildren<QAbstractButton*>()) {
            if (button->property("kvRole") == QStringLiteral("primaryAction"))
                return button;
        }
        return nullptr;
    };
    const auto save = [&] {
        auto* page = settingsPage;
        QVERIFY(saveButton()->isEnabled());
        saveButton()->click();
        // Saving rebuilds the settings page from the stored values.
        QTRY_VERIFY(window.findChild<ketplus::SettingsWorkspacePage*>() != page);
        openTypography();
    };
    constexpr int widePixels = 864;
    openTypography();
    const int defaultIndex = widthIndex(ketplus::AgentWorkspaceView::defaultConversationWidth);
    QVERIFY(widthControl() != nullptr);
    QCOMPARE(widthControl()->options().size(), ketplus::AgentConversationWidth::choices().size());
    QTRY_VERIFY(widthControl()->isVisible());
    QCOMPARE(widthControl()->currentIndex(), defaultIndex);
    QVERIFY(saveButton() != nullptr);
    QVERIFY(!saveButton()->isEnabled());
    QVERIFY(saveButton()->isVisible());

    // A width alone is a change to save, and choosing the saved one again is not.
    selectWidth(widePixels);
    QCOMPARE(widthControl()->currentIndex(), widthIndex(widePixels));
    QVERIFY(widthControl()->currentIndex() != defaultIndex);
    QVERIFY(saveButton()->isEnabled());
    QVERIFY(!QSettings().contains(QStringLiteral("agent/conversationWidth")));
    QCOMPARE(agent->conversationWidth(), ketplus::AgentWorkspaceView::defaultConversationWidth);
    selectWidth(ketplus::AgentWorkspaceView::defaultConversationWidth);
    QVERIFY(!saveButton()->isEnabled());

    selectWidth(widePixels);
    save();
    QCOMPARE(QSettings().value(QStringLiteral("agent/conversationWidth")).toInt(), widePixels);
    QCOMPARE(agent->conversationWidth(), widePixels);
    QCOMPARE(atlasChat.conversationWidth(), ketplus::AgentWorkspaceView::defaultConversationWidth);
    QCOMPARE(widthControl()->currentIndex(), widthIndex(widePixels));
    QVERIFY(!saveButton()->isEnabled());

    selectWidth(0);
    save();
    QCOMPARE(ketplus::AgentConversationWidth::load(), 0);
    QCOMPARE(agent->conversationWidth(), 0);

    // Reset defaults covers the width too; going back to the default leaves
    // nothing behind in the settings file.
    for (auto* button : settingsPage->findChildren<QAbstractButton*>()) {
        if (button->property("kvRole") == QStringLiteral("settingsReset"))
            button->click();
    }
    QCOMPARE(widthControl()->currentIndex(), defaultIndex);
    save();
    QVERIFY(!QSettings().contains(QStringLiteral("agent/conversationWidth")));
    QCOMPARE(agent->conversationWidth(), ketplus::AgentWorkspaceView::defaultConversationWidth);

    // Wide was 960 px; a saved choice of it stays Wide.
    QSettings().setValue(QStringLiteral("agent/conversationWidth"), 960);
    QCOMPARE(ketplus::AgentConversationWidth::load(), widePixels);
}

void MainWindowSettingsTest::updatesPageAndReminderDriveTheUpdater() {
    QTemporaryDir settingsDirectory;
    QTemporaryDir workspace;
    QVERIFY(settingsDirectory.isValid());
    QVERIFY(workspace.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    FakeAppUpdater updater(true);
    const ScopedAppUpdater scopedUpdater(updater);
    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    window.openFolder(workspace.path());

    auto* menuCheck = window.findChild<QAction*>(QStringLiteral("checkForUpdatesAction"));
    QVERIFY(menuCheck != nullptr);
    QCOMPARE(menuCheck->menuRole(), QAction::ApplicationSpecificRole);
    QVERIFY(menuCheck->isEnabled());
    menuCheck->trigger();
    QCOMPARE(updater.checks_, 1);
    updater.setCanCheck(false);
    QVERIFY(!menuCheck->isEnabled());
    updater.setCanCheck(true);
    QVERIFY(menuCheck->isEnabled());

    auto* userMenuCheck =
        window.findChild<QAction*>(QStringLiteral("productCheckForUpdatesAction"));
    QVERIFY(userMenuCheck != nullptr);
    userMenuCheck->trigger();
    QCOMPARE(updater.checks_, 2);

    // A scheduled check found an update while KetPlus was not in focus.
    auto* reminder = window.findChild<QAbstractButton*>(QStringLiteral("productUpdateReadyButton"));
    QVERIFY(reminder != nullptr);
    QVERIFY(reminder->isHidden());
    updater.remind(QStringLiteral("0.1.18"));
    QVERIFY(!reminder->isHidden());
    QVERIFY(reminder->accessibleDescription().contains(QStringLiteral("KétPlus Pro 0.1.18")));
    reminder->click();
    QCOMPARE(updater.checks_, 3);
    updater.remind({});
    QVERIFY(reminder->isHidden());

    window.findChild<QAction*>(QStringLiteral("productSettingsAction"))->trigger();
    QListWidget* navigation = nullptr;
    QTRY_VERIFY((navigation = window.findChild<QListWidget*>(
                     QStringLiteral("settingsWorkspaceNavigation"))) != nullptr &&
                navigation->isVisible());
    navigation->setCurrentRow(navigation->count() - 1);
    QWidget* page = nullptr;
    QTRY_VERIFY((page = window.findChild<QWidget*>(QStringLiteral("settingsWorkspaceUpdates"))) !=
                    nullptr &&
                page->isVisible());
    auto* status = page->findChild<QLabel*>(QStringLiteral("updatesStatus"));
    auto* checkNow = page->findChild<QAbstractButton*>(QStringLiteral("updatesCheckNow"));
    auto* automaticChecks =
        page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticChecks"));
    auto* automaticDownloads =
        page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticDownloads"));
    QVERIFY(status != nullptr);
    QVERIFY(checkNow != nullptr);
    QVERIFY(automaticChecks != nullptr);
    QVERIFY(automaticDownloads != nullptr);
    QCOMPARE(status->text(), QStringLiteral("KetPlus has not checked for updates yet."));
    QVERIFY(checkNow->isEnabled());
    QVERIFY(automaticChecks->isChecked());
    QVERIFY(automaticDownloads->isEnabled());
    QVERIFY(!automaticDownloads->isChecked());

    checkNow->click();
    QCOMPARE(updater.checks_, 4);
    automaticDownloads->click();
    QVERIFY(updater.automaticallyDownloadsUpdates());
    automaticChecks->click();
    QVERIFY(!updater.automaticallyChecksForUpdates());
    QVERIFY(!automaticDownloads->isEnabled());

    updater.setCanCheck(false);
    QVERIFY(!checkNow->isEnabled());
    updater.remind(QStringLiteral("0.1.18"));
    QVERIFY(status->text().contains(QStringLiteral("KetPlus 0.1.18 is available")));
    updater.remind({});
    updater.lastCheck_ = QDateTime::currentDateTime();
    updater.setCanCheck(true);
    QVERIFY(status->text().startsWith(QStringLiteral("Last checked ")));
}

void MainWindowSettingsTest::unconfiguredBuildExplainsUpdatesInsteadOfChecking() {
    QTemporaryDir settingsDirectory;
    QTemporaryDir workspace;
    QVERIFY(settingsDirectory.isValid());
    QVERIFY(workspace.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    FakeAppUpdater updater(false);
    const ScopedAppUpdater scopedUpdater(updater);
    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    window.openFolder(workspace.path());

    auto* menuCheck = window.findChild<QAction*>(QStringLiteral("checkForUpdatesAction"));
    QVERIFY(menuCheck != nullptr);
    QVERIFY(menuCheck->isEnabled());
    menuCheck->trigger();
    QCOMPARE(updater.checks_, 0);
    QWidget* page = nullptr;
    QTRY_VERIFY((page = window.findChild<QWidget*>(QStringLiteral("settingsWorkspaceUpdates"))) !=
                    nullptr &&
                page->isVisible());
    QVERIFY(page->findChild<QLabel*>(QStringLiteral("updatesStatus"))
                ->text()
                .startsWith(QStringLiteral("Updates are not configured for this build.")));
    QVERIFY(!page->findChild<QAbstractButton*>(QStringLiteral("updatesCheckNow"))->isEnabled());
    QVERIFY(
        !page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticChecks"))->isEnabled());
    QVERIFY(!page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticDownloads"))
                 ->isEnabled());
    // Opening Settings again must keep the requested section rather than reset to Appearance.
    auto* navigation =
        window.findChild<QListWidget*>(QStringLiteral("settingsWorkspaceNavigation"));
    QVERIFY(navigation != nullptr);
    QTest::qWait(20);
    QCOMPARE(navigation->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("updates"));
}

QTEST_MAIN(MainWindowSettingsTest)
#include "MainWindowSettingsTest.moc"
