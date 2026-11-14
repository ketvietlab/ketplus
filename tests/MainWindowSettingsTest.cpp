#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetChoice.h"
#include "KetSegmentedControl.h"
#include "KetThemeCard.h"
#include "KetToast.h"
#include "KetTokens.h"
#include "KetUpdateCard.h"
#include "agent/AgentWorkspaceView.h"
#include "app/AgentConversationWidth.h"
#include "app/AppearanceSettings.h"
#include "app/ArchivedSettingsPage.h"
#include "app/MainWindow.h"
#include "app/SettingsBrowserPage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsProvidersPage.h"
#include "app/SettingsWorkspacePage.h"
#include "editor/EditorWidget.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalView.h"
#include "ui/Theme.h"
#include "update/AppUpdater.h"
#include "workspace/ActivityBar.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTableView>
#include <QThread>

#include "app/PixelSelect.h"
#include "app/ProviderSetupCard.h"
#include <QAbstractButton>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimeZone>
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
    void remind(const QString& version, const bool downloaded = false) {
        setPendingVersion(version, downloaded);
    }

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
    void settingsUsesWorkspacePage();
    void themeSwitchRecoloursAnOpenDiff();
    void browserPageDefaultsOnAndPersistsChoices();
    void providersPageShowsRuntimeStatus();
    void providersPageGuidesAProviderThatCannotWork();
    void conversationWidthFollowsOnlyTheAgentWorkspace();
    void returningAFieldToItsOpeningValueSavesIt();
    void updatesPageAndReminderDriveTheUpdater();
    void hiddenStatusMessagesShowAsToasts();
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
    QList<bool> rechecks;
    ketplus::SettingsProvidersPage page([&fetches, &rechecks, providers](bool recheck, auto reply) {
        rechecks.append(recheck);
        ++fetches;
        reply(providers, {});
    });
    QSignalSpy refreshed(&page, &ketplus::SettingsProvidersPage::refreshed);
    QTRY_COMPARE(refreshed.count(), 1);
    QCOMPARE(page.providers().size(), 3);
    auto* codex = page.findChild<QWidget*>(QStringLiteral("providerRuntime-codex"));
    QVERIFY(codex != nullptr);
    auto* codexBadge = codex->findChild<ket::Badge*>(QStringLiteral("providerRuntimeStatus"));
    QCOMPARE(codexBadge->text(), QStringLiteral("Update required"));
    QCOMPARE(codexBadge->tone(), ket::BadgeTone::Danger);
    QCOMPARE(codex->findChild<QLabel*>(QStringLiteral("providerRuntimeUpdate"))->text(),
             QStringLiteral("npm install -g @openai/codex@latest"));
    auto* claude = page.findChild<QWidget*>(QStringLiteral("providerRuntime-claude"));
    QVERIFY(claude != nullptr);
    QVERIFY(claude->findChild<QLabel*>(QStringLiteral("providerRuntimeVersion"))
                ->text()
                .contains(QStringLiteral("Claude Code 2.1.1")));
    QVERIFY(claude->findChild<QLabel*>(QStringLiteral("providerRuntimeUpdate")) == nullptr);
    QCOMPARE(claude->findChild<ket::Badge*>(QStringLiteral("providerRuntimeStatus"))->text(),
             QStringLiteral("Ready")); // the mock's badge for the bundled runtime
    auto* cursor = page.findChild<QWidget*>(QStringLiteral("providerRuntime-cursor"));
    QVERIFY(cursor != nullptr);
    QVERIFY(cursor->findChild<QLabel*>(QStringLiteral("providerRuntimeVersion")) == nullptr);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("providersStatus"))->text(),
             QStringLiteral("1 provider(s) need attention."));

    page.findChild<QAbstractButton*>(QStringLiteral("providersRecheck"))->click();
    QTRY_COMPARE(refreshed.count(), 2);
    QCOMPARE(fetches, 2);
    // Opening reads; Check again asks the daemon to probe again (ADR 0110).
    QCOMPARE(rechecks, (QList<bool>{false, true}));
    // A refresh rebuilds the rows instead of appending to them.
    QCOMPARE(page.findChildren<QWidget*>(QStringLiteral("providerRuntime-codex")).size(), 1);

    ketplus::SettingsProvidersPage failing(
        [](bool, auto reply) { reply({}, QStringLiteral("daemon offline")); });
    QTRY_VERIFY(failing.findChild<QLabel*>(QStringLiteral("providersStatus"))
                    ->text()
                    .contains(QStringLiteral("daemon offline")));
}

void MainWindowSettingsTest::providersPageGuidesAProviderThatCannotWork() {
    // ADR 0110: readiness decides the guide; the page shows it in place of the version rows.
    QJsonArray providers{
        QJsonObject{{"provider", "codex"},
                    {"name", "Codex CLI"},
                    {"status", "supported"},
                    {"version", "0.150.0"},
                    {"readiness", QJsonObject{{"state", "ready"}, {"account", "dev@example.com"}}}},
        QJsonObject{{"provider", "claude"},
                    {"name", "Claude Agent SDK"},
                    {"status", "bundled"},
                    {"version", "0.3.1"},
                    {"readiness", QJsonObject{{"state", "expired"},
                                              {"message", "OAuth token has expired"},
                                              {"login_command", "claude"}}}},
        QJsonObject{{"provider", "cursor"},
                    {"name", "Cursor CLI"},
                    {"status", "missing"},
                    {"readiness",
                     QJsonObject{{"state", "missing"},
                                 {"install_command", "curl https://cursor.com/install -fsS | bash"},
                                 {"login_command", "cursor-agent login"}}}}};
    QList<bool> rechecks;
    ketplus::SettingsProvidersPage page([&rechecks, &providers](bool recheck, auto reply) {
        rechecks.append(recheck);
        reply(providers, {});
    });
    QSignalSpy refreshed(&page, &ketplus::SettingsProvidersPage::refreshed);
    QTRY_COMPARE(refreshed.count(), 1);
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("providersStatus"))->text(),
             QStringLiteral("2 provider(s) need attention."));

    // A ready provider keeps its rows and has no guide.
    auto* codex = page.findChild<QWidget*>(QStringLiteral("providerRuntime-codex"));
    QVERIFY(codex->findChild<ketplus::ProviderSetupCard*>() == nullptr);

    auto* cursor = page.findChild<QWidget*>(QStringLiteral("providerRuntime-cursor"));
    auto* cursorBadge = cursor->findChild<ket::Badge*>(QStringLiteral("providerRuntimeStatus"));
    QCOMPARE(cursorBadge->text(), QStringLiteral("Not installed"));
    QCOMPARE(cursorBadge->property("setupId").toString(), QStringLiteral("cli-missing"));
    auto* cursorSetup = cursor->findChild<ketplus::ProviderSetupCard*>();
    QVERIFY(cursorSetup != nullptr);
    QCOMPARE(cursorSetup->guideId(), QStringLiteral("cli-missing"));
    QStringList commands;
    for (auto* label :
         cursorSetup->findChildren<QLabel*>(QStringLiteral("providerSetupCommandText")))
        commands << label->text();
    QVERIFY2(commands.contains(QStringLiteral("curl https://cursor.com/install -fsS | bash")),
             qPrintable(commands.join(u'|')));
    QVERIFY(cursorSetup->findChildren<QLabel*>(QStringLiteral("providerSetupStep")).size() >= 2);

    // Copy and Type in Terminal hand the command over; nothing runs it.
    QSignalSpy copied(cursorSetup, &ketplus::ProviderSetupCard::commandCopied);
    cursorSetup->findChildren<QAbstractButton*>(QStringLiteral("providerSetupCopy"))
        .first()
        ->click();
    QCOMPARE(copied.count(), 1);
    QCOMPARE(QApplication::clipboard()->text(), copied.first().first().toString());
    QSignalSpy typed(&page, &ketplus::SettingsProvidersPage::typeInTerminalRequested);
    cursorSetup->findChildren<QAbstractButton*>(QStringLiteral("providerSetupTypeInTerminal"))
        .first()
        ->click();
    QCOMPARE(typed.count(), 1);
    QCOMPARE(typed.first().first().toString(), copied.first().first().toString());

    auto* claude = page.findChild<QWidget*>(QStringLiteral("providerRuntime-claude"));
    auto* claudeSetup = claude->findChild<ketplus::ProviderSetupCard*>();
    QVERIFY(claudeSetup != nullptr);
    QCOMPARE(claudeSetup->guideId(), QStringLiteral("expired"));
    QCOMPARE(claudeSetup->findChild<QLabel*>(QStringLiteral("providerSetupError"))->text(),
             QStringLiteral("OAuth token has expired"));
    QCOMPARE(claudeSetup->findChild<QLabel*>(QStringLiteral("providerSetupError"))->textFormat(),
             Qt::PlainText);

    if (const auto directory = qEnvironmentVariable("KETPLUS_SETTINGS_CAPTURE_DIR");
        !directory.isEmpty()) {
        page.resize(760, 2400);
        page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        for (const auto mode : {ket::Appearance::Mode::Light, ket::Appearance::Mode::Dark}) {
            ket::Appearance::instance().setMode(mode);
            QCoreApplication::processEvents();
            QVERIFY(page.grab().save(QDir(directory).filePath(
                QStringLiteral("providers-setup-%1.png")
                    .arg(mode == ket::Appearance::Mode::Dark ? QStringLiteral("dark")
                                                             : QStringLiteral("light")))));
        }
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
    }

    // Check again probes; a guide that comes back says it is still failing.
    QSignalSpy still(&page, &ketplus::SettingsProvidersPage::refreshed);
    // Only the guide that was checked says so (the mock's cli-missing `still`).
    auto* checkCursor =
        cursorSetup->findChild<QAbstractButton*>(QStringLiteral("providerSetupPrimary"));
    QCOMPARE(checkCursor->text(), QStringLiteral("Check again"));
    checkCursor->click();
    QTRY_COMPARE(still.count(), 1);
    QCOMPARE(rechecks, (QList<bool>{false, true}));
    cursor = page.findChild<QWidget*>(QStringLiteral("providerRuntime-cursor"));
    QVERIFY(cursor->findChild<QLabel*>(QStringLiteral("providerSetupStill"))->isVisibleTo(cursor));
    claude = page.findChild<QWidget*>(QStringLiteral("providerRuntime-claude"));
    QVERIFY(!claude->findChild<QLabel*>(QStringLiteral("providerSetupStill"))->isVisibleTo(claude));

    // Once the sign-in works, the guide goes away.
    QJsonObject fixed = providers.at(1).toObject();
    fixed.insert("readiness", QJsonObject{{"state", "ready"}});
    providers.replace(1, fixed);
    page.findChild<QAbstractButton*>(QStringLiteral("providersRecheck"))->click();
    QTRY_COMPARE(still.count(), 2);
    claude = page.findChild<QWidget*>(QStringLiteral("providerRuntime-claude"));
    QVERIFY(claude->findChild<ketplus::ProviderSetupCard*>() == nullptr);
    QCOMPARE(claude->findChild<ket::Badge*>(QStringLiteral("providerRuntimeStatus"))->text(),
             QStringLiteral("Ready"));
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
    auto* all = page.findChild<QAbstractButton*>(QStringLiteral("browserToolsAllOrigins"));
    auto* only = page.findChild<QAbstractButton*>(QStringLiteral("browserToolsOnlyOrigins"));
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
    // The mock's layout: choices, 200 px fields, and the one-line status keeps its text in
    // the tooltip.
    QVERIFY(qobject_cast<ket::Choice*>(enabled) && qobject_cast<ket::Choice*>(all) &&
            qobject_cast<ket::Choice*>(only));
    QCOMPARE(origins->width(), 200);
    QCOMPARE(status->toolTip(), status->text());
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
    QVERIFY(mixedPage.findChild<QAbstractButton*>(QStringLiteral("browserToolsAllOrigins"))
                ->isChecked());
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
    archived[0] = [&] {
        auto first = archived.at(0).toMap();
        first.insert(QStringLiteral("number"), QStringLiteral("1022"));
        first.insert(QStringLiteral("archived_at"),
                     QDateTime(QDate(2026, 10, 28), QTime(9, 0), QTimeZone::UTC));
        return QVariant(first);
    }();
    QString stopped;
    QString restored;
    QString deleted;
    ketplus::ArchivedSettingsPage page(
        archived,
        [&restored](const QString&, const QString& id) {
            restored = id;
            return true;
        },
        [&stopped](const QString&, const QString& id) {
            stopped = id;
            return true;
        },
        [&deleted](const QString&, const QString& id) { deleted = id; });
    page.resize(760, 400);
    page.show();
    const auto cards = [&page] {
        QList<QWidget*> visible;
        for (auto* card : page.findChildren<QWidget*>(QStringLiteral("archivedItem")))
            if (card->parent() != nullptr && !card->isHidden())
                visible.append(card);
        return visible;
    };
    // The mock's card: the title, then the number, folder and archive date.
    QTRY_COMPARE(cards().size(), 2);
    auto* first = cards().first();
    const auto details = first->findChild<QLabel*>(QStringLiteral("settingsWorkspaceHelp"));
    QVERIFY(details->text().startsWith(QStringLiteral("@agent/1022 · workspace · archived ")));
    QVERIFY(details->text().endsWith(QStringLiteral(" · Running")));
    auto* stop = first->findChild<QAbstractButton*>(QStringLiteral("stopArchivedTurnButton"));
    QVERIFY(stop != nullptr);
    // A worktree has Restore only, and no Stop while nothing runs.
    QVERIFY(cards().last()->findChild<QAbstractButton*>(
                QStringLiteral("deleteArchivedItemButton")) == nullptr);
    QVERIFY(cards().last()->findChild<QAbstractButton*>(QStringLiteral("stopArchivedTurnButton")) ==
            nullptr);

    if (const auto capture = qEnvironmentVariable("KETPLUS_ARCHIVED_CAPTURE"); !capture.isEmpty())
        QVERIFY(page.grab().save(capture));
    stop->click();
    QCOMPARE(stopped, QStringLiteral("conversation-1"));
    QTRY_COMPARE(cards().size(), 2); // rebuilt; a new card shows once the event loop runs
    QVERIFY(cards().first()->findChild<QAbstractButton*>(
                QStringLiteral("stopArchivedTurnButton")) == nullptr);
    cards()
        .first()
        ->findChild<QAbstractButton*>(QStringLiteral("deleteArchivedItemButton"))
        ->click();
    QCOMPARE(deleted, QStringLiteral("conversation-1"));
    cards()
        .first()
        ->findChild<QAbstractButton*>(QStringLiteral("restoreArchivedItemButton"))
        ->click();
    QCOMPARE(restored, QStringLiteral("conversation-1"));
    QTRY_COMPARE(cards().size(), 1);
    page.setConversations({});
    QTRY_COMPARE(cards().size(), 0);
    QVERIFY(page.findChild<QWidget*>(QStringLiteral("archivedEmptyState"))->isVisible());
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
        window.findChild<ketplus::PixelSelect*>(QStringLiteral("editorFontSize"))->setValue(17);
        window.findChild<ketplus::PixelSelect*>(QStringLiteral("editorLineHeight"))->setValue(29);
        window.findChild<QAbstractButton*>(QStringLiteral("followSystemTheme"))->setChecked(false);
        window.findChild<QAbstractButton*>(QStringLiteral("darkThemeCard"))->click();
        // Every change applied as it was made; there is no Save button.
        QVERIFY(window.findChild<QAbstractButton*>(QStringLiteral("settingsSaveButton")) ==
                nullptr);
        saved = true;
    });
    settingsAction->trigger();
    QTRY_VERIFY(saved);
    QTRY_VERIFY(window.findChild<ketplus::SettingsWorkspacePage*>() != nullptr);
    auto* embeddedSettings = window.findChild<ketplus::SettingsDialog*>();
    QVERIFY(embeddedSettings != nullptr);
    QVERIFY(!embeddedSettings->isWindow());
    QVERIFY(!embeddedSettings->isModal());

    const auto appearance = ketplus::AppearanceSettings::load();
    QCOMPARE(appearance.interfaceZoomPercent, 110);
    QCOMPARE(appearance.interfaceFontSizePixels, 14);
    QCOMPARE(appearance.editor.fontSizePixels, 17);
    QCOMPARE(appearance.editor.lineHeightPixels, 29);
    // Terminal text follows the editor, and Markdown reads a pixel over the interface at the
    // zoom: 15 px at 110%.
    QCOMPARE(appearance.terminal, (ketplus::TextTypographySettings{17, 29}));
    QCOMPARE(appearance.preview, (ketplus::TextTypographySettings{17, 29}));
    QCOMPARE(theme.interfaceZoomPercent(), 110);
    QCOMPARE(theme.interfaceFontSizePixels(), 14);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
    QVERIFY(actionWithText(window, QStringLiteral("Dark"))->isChecked());
    QVERIFY(!actionWithText(window, QStringLiteral("System"))->isChecked());

    const auto editors = window.findChildren<ketplus::EditorWidget*>();
    QVERIFY(!editors.isEmpty());
    for (const auto* editor : editors) {
        QCOMPARE(editor->editorSettings(), appearance.editor);
    }
    for (const auto* terminal : window.findChildren<ketplus::TerminalView*>()) {
        QCOMPARE(terminal->font().pixelSize(), 17);
    }

    auto* preview = window.findChild<ketplus::MarkdownPreviewPane*>();
    QVERIFY(preview != nullptr);
    auto* browser = preview->findChild<QTextBrowser*>(QStringLiteral("markdownPreviewBrowser"));
    QVERIFY(browser != nullptr);
    QTRY_VERIFY(
        browser->document()->defaultStyleSheet().contains(QStringLiteral("font-size: 17px")));
}

void MainWindowSettingsTest::themeSwitchRecoloursAnOpenDiff() {
    // #142 / #100: switching the theme recolours an open diff tab without reopening it.
    QTemporaryDir settingsDirectory;
    QTemporaryDir workspace;
    QVERIFY(settingsDirectory.isValid());
    QVERIFY(workspace.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::MainWindow window(theme);
    window.show();
    window.openFolder(workspace.path());
    auto* git = window.findChild<ketplus::GitService*>();
    QVERIFY(git != nullptr);
    const QString file = QDir(workspace.path()).filePath(QStringLiteral("example.cpp"));
    emit git->diffReady(file, ketplus::GitDiffMode::Unstaged,
                        QStringLiteral("diff --git a/example.cpp b/example.cpp\n"
                                       "--- a/example.cpp\n+++ b/example.cpp\n"
                                       "@@ -1,2 +1,2 @@\n int unchanged = 0;\n"
                                       "-int value = 1;\n+int value = 2;\n"),
                        {});
    ketplus::GitDiffView* diff = nullptr;
    QTRY_VERIFY((diff = window.findChild<ketplus::GitDiffView*>()) != nullptr);
    auto* table = diff->findChild<QTableView*>();
    QVERIFY(table != nullptr && table->model() != nullptr);
    const auto contextBackground = [table] {
        auto* model = table->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            for (int column = 0; column < model->columnCount(); ++column) {
                const QModelIndex index = model->index(row, column);
                if (index.data(Qt::DisplayRole).toString().contains(QStringLiteral("unchanged")))
                    return index.data(Qt::BackgroundRole).value<QColor>();
            }
        }
        return QColor();
    };
    QCOMPARE(contextBackground(), QColor(ket::Tokens::color(u"panel-bg", ket::Theme::Light)));
    theme.setMode(ketplus::ThemeManager::Mode::Dark);
    QTRY_COMPARE(contextBackground(), QColor(ket::Tokens::color(u"panel-bg", ket::Theme::Dark)));
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    QTRY_COMPARE(contextBackground(), QColor(ket::Tokens::color(u"panel-bg", ket::Theme::Light)));
}

void MainWindowSettingsTest::settingsUsesWorkspacePage() {
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
    // Worktrees have no page of their own; they are managed from a conversation.
    QVERIFY(window.findChild<QAction*>(QStringLiteral("manageWorktreesAction")) == nullptr);
    auto* agentButton =
        window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"));
    QVERIFY(agentButton != nullptr);
    agentButton->click();
    QCOMPARE(activity->mode(), ketplus::ActivityBar::Mode::AgentWorkspace);

    auto* settingsAction = window.findChild<QAction*>(QStringLiteral("productSettingsAction"));
    QVERIFY(settingsAction != nullptr);
    settingsAction->trigger();
    ketplus::SettingsWorkspacePage* settingsPage = nullptr;
    QTRY_VERIFY((settingsPage = window.findChild<ketplus::SettingsWorkspacePage*>()) != nullptr &&
                settingsPage->isVisible());
    QTRY_VERIFY(settingsPage->width() >= 320);
    QTRY_VERIFY(settingsPage->height() >= 240);
    QCOMPARE(activity->mode(), ketplus::ActivityBar::Mode::Settings);
    QVERIFY(!window.statusBar()->isVisible());
    auto* settingsNavigation =
        settingsPage->findChild<QListWidget*>(QStringLiteral("settingsWorkspaceNavigation"));
    QVERIFY(settingsNavigation != nullptr);
    QCOMPARE(settingsNavigation->count(), 11);
    QCOMPARE(settingsNavigation->item(9)->data(Qt::UserRole).toString(), QStringLiteral("storage"));
    auto* storageShortcut =
        activity->findChild<QAbstractButton*>(QStringLiteral("sidebarStorageSettings"));
    QVERIFY(storageShortcut != nullptr);
    storageShortcut->click();
    QTRY_VERIFY(
        settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspaceStorage"))->isVisible());
    QCOMPARE(settingsNavigation->item(10)->data(Qt::UserRole).toString(),
             QStringLiteral("updates"));
    // ADR 0114, open point 2: Teams follows Agent models, as in the mock.
    QCOMPARE(settingsNavigation->item(4)->data(Qt::UserRole).toString(), QStringLiteral("teams"));
    QCOMPARE(settingsNavigation->item(4)->text(), QStringLiteral("Teams"));
    QCOMPARE(settingsNavigation->item(5)->data(Qt::UserRole).toString(), QStringLiteral("browser"));
    QCOMPARE(settingsNavigation->item(5)->text(), QStringLiteral("Browser"));
    QCOMPARE(settingsNavigation->item(6)->data(Qt::UserRole).toString(),
             QStringLiteral("computer"));
    QCOMPARE(settingsNavigation->item(6)->text(), QStringLiteral("Computer use"));
    QCOMPARE(settingsNavigation->item(7)->data(Qt::UserRole).toString(),
             QStringLiteral("providers"));
    auto* computerPage =
        settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspaceComputer"));
    QVERIFY(computerPage != nullptr);
    settingsNavigation->setCurrentRow(6);
    QTRY_VERIFY(computerPage->isVisible());
    auto* providersPage =
        settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspaceProviders"));
    QVERIFY(providersPage != nullptr);
    settingsNavigation->setCurrentRow(7);
    QTRY_VERIFY(providersPage->isVisible());
    QVERIFY(!computerPage->isVisible());
    settingsNavigation->setCurrentRow(0);
    QTRY_VERIFY(!providersPage->isVisible());
    QCOMPARE(settingsNavigation->currentItem()->data(Qt::UserRole).toString(),
             QStringLiteral("appearance"));
    auto* lightTheme = settingsPage->findChild<ket::ThemeCard*>(QStringLiteral("lightThemeCard"));
    auto* darkTheme = settingsPage->findChild<ket::ThemeCard*>(QStringLiteral("darkThemeCard"));
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
    QVERIFY(settingsPage->findChild<QWidget*>(QStringLiteral("settingsWorkspacePreview")) !=
            nullptr);
    QCOMPARE(lightTheme->text(), QStringLiteral("Light"));
    QCOMPARE(darkTheme->text(), QStringLiteral("Dark"));
    QCOMPARE(lightTheme->width(), darkTheme->width());
    QVERIFY(lightTheme->width() >= 264);
    QVERIFY(darkTheme->isVisible());
    darkTheme->click();
    QTRY_VERIFY(theme.palette().dark);
    lightTheme->click();
    QTRY_VERIFY(!theme.palette().dark);
    // Following the system keeps the card of the theme in use marked.
    auto* followSystem =
        settingsPage->findChild<QAbstractButton*>(QStringLiteral("followSystemTheme"));
    QVERIFY(followSystem != nullptr);
    followSystem->click();
    QVERIFY(followSystem->isChecked());
    QCOMPARE(lightTheme->isChecked(), !theme.palette().dark);
    QCOMPARE(darkTheme->isChecked(), theme.palette().dark);
    followSystem->click();
    QVERIFY(!followSystem->isChecked());
    // Appearance has the mock's zoom steps (ADR 0104).
    auto* zoom = window.findChild<ket::SegmentedControl*>(QStringLiteral("interfaceZoom"));
    QVERIFY(zoom != nullptr);
    QTRY_VERIFY(zoom->isVisible());
    QCOMPARE(zoom->options(), QStringList({"90%", "100%", "110%", "125%"}));
    QCOMPARE(zoom->currentIndex(), 1);
    zoom->setCurrentIndex(2);
    QCOMPARE(ketplus::AppearanceSettings::load().interfaceZoomPercent, 110);
    QCOMPARE(theme.interfaceZoomPercent(), 110);
    // Typography has the interface font size, which leaves the zoom.
    settingsNavigation->setCurrentRow(1);
    auto* interfaceSize =
        window.findChild<ketplus::PixelSelect*>(QStringLiteral("interfaceFontSize"));
    QVERIFY(interfaceSize != nullptr);
    QTRY_VERIFY(interfaceSize->isVisible());
    QCOMPARE(interfaceSize->minimum(), 13);
    QCOMPARE(interfaceSize->maximum(), 16);
    interfaceSize->setValue(16);
    QCOMPARE(ketplus::AppearanceSettings::load().interfaceFontSizePixels, 16);
    QCOMPARE(ketplus::AppearanceSettings::load().interfaceZoomPercent, 110);
    QCOMPARE(theme.interfaceTextPixels(), 18);
    QCOMPARE(QApplication::font().pixelSize(), 18);
    QCOMPARE(ket::Appearance::instance().zoom(), 1.1);
    interfaceSize->setValue(14);
    QVERIFY(settingsPage->findChild<QComboBox*>(QStringLiteral("settingsInterfaceFont")) !=
            nullptr);
    auto* preview = settingsPage->findChild<QWidget*>(QStringLiteral("typographyPreview"));
    QVERIFY(preview && preview->isVisible());
    auto* editorSize =
        settingsPage->findChild<ketplus::PixelSelect*>(QStringLiteral("editorFontSize"));
    // The sample shows the interface text at the zoom; Restore defaults returns it to 100%.
    const int zoomedHeight = preview->minimumHeight();
    editorSize->setValue(32);
    QVERIFY(preview->minimumHeight() > zoomedHeight);
    window.findChild<QAbstractButton*>(QStringLiteral("settingsResetButton"))->click();
    QCOMPARE(editorSize->value(), 14);
    QCOMPARE(zoom->currentIndex(), 1);
    QCOMPARE(ketplus::AppearanceSettings::load().interfaceZoomPercent, 100);
    QCOMPARE(preview->minimumHeight(), zoomedHeight - 1);
    QCoreApplication::processEvents();
    QCOMPARE(settingsNavigation->currentItem()->data(Qt::UserRole).toString(),
             QStringLiteral("typography"));
    if (const auto capture = qEnvironmentVariable("KETPLUS_SETTINGS_REVIEW_CAPTURE");
        !capture.isEmpty()) {
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(capture + ".light.png"));
        darkTheme->click();
        QCoreApplication::processEvents();
        QVERIFY(window.grab().save(capture + ".dark.png"));
    }
}

// Changes apply one at a time, so a field taken back to the value it had when Settings
// opened must be saved too, by a direct edit or by Restore defaults.
void MainWindowSettingsTest::returningAFieldToItsOpeningValueSavesIt() {
    QTemporaryDir settingsDirectory;
    QVERIFY(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();

    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    window.findChild<QAction*>(QStringLiteral("productSettingsAction"))->trigger();
    ketplus::PixelSelect* size = nullptr;
    QTRY_VERIFY((size = window.findChild<ketplus::PixelSelect*>(
                     QStringLiteral("editorFontSize"))) != nullptr);
    const int opening = size->value();
    size->setValue(opening + 2);
    QCOMPARE(ketplus::AppearanceSettings::load().editor.fontSizePixels, opening + 2);
    size->setValue(opening);
    QCOMPARE(ketplus::AppearanceSettings::load().editor.fontSizePixels, opening);

    size->setValue(opening + 3);
    QCOMPARE(ketplus::AppearanceSettings::load().editor.fontSizePixels, opening + 3);
    for (auto* button : window.findChildren<QAbstractButton*>()) {
        if (button->property("kvRole") == QStringLiteral("settingsReset"))
            button->click();
    }
    QCOMPARE(ketplus::AppearanceSettings::load().editor.fontSizePixels,
             ketplus::AppearanceSettings::defaults().editor.fontSizePixels);
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
    window.findChild<QAbstractButton*>(QStringLiteral("agentWorkspaceActivityButton"))->click();
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
        return settingsPage->findChild<QComboBox*>(QStringLiteral("agentConversationWidth"));
    };
    const auto widthIndex = [](int pixels) {
        const auto choices = ketplus::AgentConversationWidth::choices();
        for (int i = 0; i < choices.size(); ++i)
            if (choices[i].pixels == pixels)
                return i;
        return -1;
    };
    const auto selectWidth = [&](int pixels) {
        widthControl()->setCurrentIndex(widthIndex(pixels));
    };
    constexpr int widePixels = 864;
    openTypography();
    const int defaultIndex = widthIndex(ketplus::AgentWorkspaceView::defaultConversationWidth);
    QVERIFY(widthControl() != nullptr);
    QCOMPARE(widthControl()->count(), ketplus::AgentConversationWidth::choices().size());
    QCOMPARE(widthControl()->itemText(0), QString::fromUtf8("Default · 760 px"));
    QTRY_VERIFY(widthControl()->isVisible());
    QCOMPARE(widthControl()->currentIndex(), defaultIndex);
    QVERIFY(!QSettings().contains(QStringLiteral("agent/conversationWidth")));

    // A width applies as it is chosen, to the Agent Workspace only.
    selectWidth(widePixels);
    QCOMPARE(QSettings().value(QStringLiteral("agent/conversationWidth")).toInt(), widePixels);
    QCOMPARE(agent->conversationWidth(), widePixels);
    QCOMPARE(atlasChat.conversationWidth(), ketplus::AgentWorkspaceView::defaultConversationWidth);
    QCOMPARE(widthControl()->currentIndex(), widthIndex(widePixels));

    selectWidth(0);
    QCOMPARE(ketplus::AgentConversationWidth::load(), 0);
    QCOMPARE(agent->conversationWidth(), 0);

    // Restore defaults covers the width too; going back to the default leaves
    // nothing behind in the settings file.
    for (auto* button : settingsPage->findChildren<QAbstractButton*>()) {
        if (button->property("kvRole") == QStringLiteral("settingsReset"))
            button->click();
    }
    QCOMPARE(widthControl()->currentIndex(), defaultIndex);
    QVERIFY(!QSettings().contains(QStringLiteral("agent/conversationWidth")));
    QCOMPARE(agent->conversationWidth(), ketplus::AgentWorkspaceView::defaultConversationWidth);

    // Wide was 960 px; a saved choice of it stays Wide.
    QSettings().setValue(QStringLiteral("agent/conversationWidth"), 960);
    QCOMPARE(ketplus::AgentConversationWidth::load(), widePixels);
}

void MainWindowSettingsTest::hiddenStatusMessagesShowAsToasts() {
    QTemporaryDir settingsDirectory;
    QVERIFY(settingsDirectory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings().clear();
    ketplus::ThemeManager theme(*qApp);
    ketplus::MainWindow window(theme);
    window.resize(1100, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto toasts = [&window] {
        QStringList texts;
        for (auto* toast : window.findChildren<ket::Toast*>()) {
            texts.append(toast->text());
            // Status messages include failures, so none reads as a success.
            if (toast->tone() != ket::ToastTone::Neutral)
                texts.append(QStringLiteral("not neutral"));
        }
        return texts;
    };

    // In Settings, as in the Agent Workspace, the status bar is hidden, so its message shows
    // as a toast.
    window.findChild<QAction*>(QStringLiteral("productSettingsAction"))->trigger();
    QTRY_VERIFY(!window.statusBar()->isVisible());
    // The idle text is not an event.
    window.statusBar()->showMessage(QStringLiteral("Ready"));
    QVERIFY(!toasts().contains(QStringLiteral("Ready")));
    window.statusBar()->showMessage(QStringLiteral("Saved the plan"), 2000);
    QVERIFY(toasts().contains(QStringLiteral("Saved the plan")));
    QVERIFY(!toasts().contains(QStringLiteral("not neutral")));
    // KETPLUS_OVERLAYS_CAPTURE=<dir> saves the toast for the side by side with
    // `KetPlusMock --open=toast`.
    if (const auto dir = qEnvironmentVariable("KETPLUS_OVERLAYS_CAPTURE"); !dir.isEmpty()) {
        QTest::qWait(400); // the toast has faded in
        QVERIFY(window.grab().save(QDir(dir).filePath(QStringLiteral("toast.png"))));
    }

    // With the status bar on screen, the message stays there.
    window.statusBar()->show();
    window.statusBar()->showMessage(QStringLiteral("Replaced 3 matches"), 2000);
    QVERIFY(!toasts().contains(QStringLiteral("Replaced 3 matches")));
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
    QVERIFY(reminder->accessibleDescription().contains(QStringLiteral("ketPlus Pro 0.1.18")));
    QVERIFY(reminder->accessibleDescription().contains(QStringLiteral("Review and install")));
    // Downloaded, it reads as the mock's "Restart to install".
    updater.remind(QStringLiteral("0.1.18"), true);
    QVERIFY(reminder->accessibleDescription().contains(QStringLiteral("Restart to install")));
    // And the reminder, with `KetPlusMock --screen=workspace`.
    if (const auto dir = qEnvironmentVariable("KETPLUS_OVERLAYS_CAPTURE"); !dir.isEmpty()) {
        updater.remind(QStringLiteral("1.8.0"), true);
        QTest::qWait(100);
        QVERIFY(window.grab().save(QDir(dir).filePath(QStringLiteral("update-pill.png"))));
        updater.remind(QStringLiteral("0.1.18"), true);
    }
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
    auto* automaticDownloads =
        page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticDownloads"));
    auto* card = page->findChild<ket::UpdateCard*>(QStringLiteral("updatesCard"));
    auto* restart = page->findChild<QAbstractButton*>(QStringLiteral("updatesRestart"));
    auto* later = page->findChild<QAbstractButton*>(QStringLiteral("updatesLater"));
    QVERIFY(status != nullptr);
    QVERIFY(checkNow != nullptr);
    QVERIFY(automaticDownloads != nullptr);
    QVERIFY(card != nullptr && restart != nullptr && later != nullptr);
    auto* automaticChecks =
        page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticChecks"));
    QVERIFY(automaticChecks != nullptr);
    const auto cardText = [card] {
        QStringList texts;
        for (auto* label : card->findChildren<QLabel*>())
            if (!label->isHidden())
                texts.append(label->text());
        return texts.join(QLatin1Char('\n'));
    };
    const auto badge = [card] { return card->findChild<ket::Badge*>()->text(); };
    QCOMPARE(status->text(), QStringLiteral("ketPlus checks for updates every day and installs "
                                            "them when you restart."));
    QVERIFY(card->isVisible());
    QVERIFY(cardText().contains(QStringLiteral("ketPlus has not checked for updates yet.")));
    QCOMPARE(badge(), QStringLiteral("Installed"));
    QVERIFY(!restart->isVisible());
    QCOMPARE(checkNow->text(), QStringLiteral("Check for updates"));
    QVERIFY(checkNow->isEnabled());
    QVERIFY(automaticDownloads->isEnabled());
    QVERIFY(!automaticDownloads->isChecked());
    QVERIFY(automaticChecks->isChecked());

    checkNow->click();
    QCOMPARE(updater.checks_, 4);
    automaticDownloads->click();
    QVERIFY(updater.automaticallyDownloadsUpdates());
    QVERIFY(updater.automaticallyChecksForUpdates());
    automaticDownloads->click();
    QVERIFY(!updater.automaticallyDownloadsUpdates());
    QVERIFY(updater.automaticallyChecksForUpdates()); // the daily check stays
    // The scheduled check can be turned off, which also stops background downloads.
    automaticDownloads->click();
    automaticChecks->click();
    QVERIFY(!updater.automaticallyChecksForUpdates());
    QVERIFY(!updater.automaticallyDownloadsUpdates());
    QVERIFY(!automaticDownloads->isChecked());
    QVERIFY(status->text().contains(QStringLiteral("when you ask")));
    // Turning downloads on also turns the scheduled check back on.
    automaticDownloads->click();
    QVERIFY(updater.automaticallyChecksForUpdates());
    QVERIFY(automaticChecks->isChecked());
    QVERIFY(automaticDownloads->isChecked());

    updater.setCanCheck(false);
    QVERIFY(!checkNow->isEnabled());
    updater.remind(QStringLiteral("0.1.18"));
    QCOMPARE(badge(), QStringLiteral("Available"));
    QVERIFY(cardText().contains(QStringLiteral("ketPlus Pro 0.1.18")));
    QVERIFY(!restart->isVisible());
    updater.setCanCheck(true);
    updater.remind(QStringLiteral("0.1.18"), true);
    QCOMPARE(badge(), QStringLiteral("Ready"));
    QVERIFY(card->isReady());
    QVERIFY(restart->isVisible() && later->isVisible());
    restart->click();
    QCOMPARE(updater.checks_, 5); // Sparkle's prompt installs and relaunches
    later->click();
    QVERIFY(!restart->isVisible());
    QVERIFY(cardText().contains(QStringLiteral("installs the next time ketPlus restarts")));
    updater.remind({});
    updater.lastCheck_ = QDateTime::currentDateTime();
    updater.setCanCheck(true);
    QCOMPARE(badge(), QStringLiteral("Up to date"));
    QVERIFY(cardText().contains(QStringLiteral("Checked ")));
    const QString capture = qEnvironmentVariable("KETPLUS_UPDATES_CAPTURE");
    if (!capture.isEmpty()) {
        updater.remind(QStringLiteral("0.1.19"), true); // a newer download shows its actions
        QVERIFY(restart->isVisible());
        QVERIFY(window.grab().save(capture));
    }
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
    QVERIFY(!page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticDownloads"))
                 ->isEnabled());
    QVERIFY(
        !page->findChild<QAbstractButton*>(QStringLiteral("updatesAutomaticChecks"))->isEnabled());
    QVERIFY(page->findChild<QWidget*>(QStringLiteral("updatesCard"))->isHidden());
    // Opening Settings again must keep the requested section rather than reset to Appearance.
    auto* navigation =
        window.findChild<QListWidget*>(QStringLiteral("settingsWorkspaceNavigation"));
    QVERIFY(navigation != nullptr);
    QTest::qWait(20);
    QCOMPARE(navigation->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("updates"));
}

QTEST_MAIN(MainWindowSettingsTest)
#include "MainWindowSettingsTest.moc"
