#include "app/SettingsBrowserPage.h"
#include "KetButton.h"
#include "KetChoice.h"
#include "KetTextField.h"
#include "KetToggle.h"

#include "ToolPackageRegistry.h"
#include "app/SettingsRows.h"

#include <QButtonGroup>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace ketplus {
namespace {

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    return value;
}

// Mirrors the launch: any "*" entry in a valid list allows every website.
bool isAllOrigins(const QString& origins) {
    return browser_tools::normalizeAllowedOrigins(origins) ==
           QLatin1String(browser_tools::allOrigins);
}

// File checks only (cheap enough for the GUI thread): the bundled runtime (when compiled in) is
// tried first; otherwise a ready prerequisite check means the source checkout.
QString runtimeSourceFor(const browser_tools::Prerequisites& prerequisites,
                         const browser_tools::Settings& settings, const QString& searchStart) {
    browser_tools::Settings launchSettings = settings;
    if (prerequisites.nodeOk)
        launchSettings.nodePath = prerequisites.nodePath;
    const auto launch = browser_tools::resolveBrowserLaunch(launchSettings, searchStart);
    const QString source = QLatin1String(browser_tools::SourceCheckoutProvider::sourceName);
    if (launch && launch->source != source)
        return launch->source;
    return prerequisites.ok() ? source : QString();
}

} // namespace

SettingsBrowserPage::SettingsBrowserPage(QString settingsPath, QString searchStart,
                                         browser_tools::Probes probes, QWidget* parent)
    : QWidget(parent), settingsPath_(std::move(settingsPath)),
      searchStart_(searchStart.isEmpty() ? QCoreApplication::applicationDirPath()
                                         : std::move(searchStart)),
      probes_(std::move(probes)), settings_(browser_tools::loadSettings(settingsPath_)) {
    setObjectName(QStringLiteral("settingsWorkspaceBrowser"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Browser"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(20);

    // The mock's layout: the runtime state beside its recheck button, the switch and the
    // website modes as choices, and each field at the end of a titled row.
    auto* runtime = new QWidget(this);
    auto* runtimeRow = new QHBoxLayout(runtime);
    runtimeRow->setContentsMargins(0, 0, 0, 0);
    runtimeRow->setSpacing(12);
    status_ =
        textLabel(tr("Checking the browser runtime…"), QStringLiteral("browserToolsStatus"), this);
    if (!settings_.loadError.isEmpty())
        status_->setText(tr("Browser settings could not be read: %1").arg(settings_.loadError));
    // One line beside the button, as in the mock; a long error is clipped, and setStatus()
    // keeps the whole text in the tooltip.
    status_->setWordWrap(false);
    status_->setMinimumWidth(1);
    status_->setToolTip(status_->text());
    runtimeRow->addWidget(status_);
    auto* recheck = new ket::Button(tr("Check again"), ket::ButtonVariant::Secondary, this);
    recheck->setObjectName(QStringLiteral("browserToolsRecheck"));
    runtimeRow->addWidget(recheck, 0, Qt::AlignVCenter);
    runtimeRow->addStretch(1);
    layout->addWidget(runtime);
    layout->addSpacing(14);

    enabled_ =
        new ket::Choice(tr("Let agents use a browser"),
                        tr("Chromium in its own profile; pages, captures and visual comparison"),
                        ket::ToggleKind::Checkbox, this);
    enabled_->setObjectName(QStringLiteral("browserToolsEnabled"));
    enabled_->setChecked(settings_.enabled);
    layout->addWidget(enabled_);
    layout->addSpacing(14);
    layout->addWidget(settings_rows::divider(this));
    layout->addSpacing(14);

    layout->addWidget(
        textLabel(tr("Websites"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(14);
    allOrigins_ = new ket::Choice(tr("All websites"), {}, ket::ToggleKind::Radio, this);
    allOrigins_->setObjectName(QStringLiteral("browserToolsAllOrigins"));
    onlyOrigins_ = new ket::Choice(tr("Only these origins"), {}, ket::ToggleKind::Radio, this);
    onlyOrigins_->setObjectName(QStringLiteral("browserToolsOnlyOrigins"));
    auto* originMode = new QButtonGroup(this);
    originMode->addButton(allOrigins_);
    originMode->addButton(onlyOrigins_);
    (isAllOrigins(settings_.allowedOrigins) ? allOrigins_ : onlyOrigins_)->setChecked(true);
    layout->addWidget(allOrigins_);
    layout->addSpacing(14);
    layout->addWidget(onlyOrigins_);
    layout->addSpacing(14);

    origins_ = new ket::TextField(this);
    origins_->setText(isAllOrigins(settings_.allowedOrigins) ? QString()
                                                             : settings_.allowedOrigins);
    origins_->setObjectName(QStringLiteral("browserToolsOrigins"));
    origins_->setPlaceholderText(QStringLiteral("loopback, https://staging.example.com"));
    appPath_ = new ket::TextField(this);
    appPath_->setText(settings_.browserAppPath);
    appPath_->setObjectName(QStringLiteral("browserToolsAppPath"));
    const QString detected = browser_tools::detectBrowserAppPath(searchStart_);
    appPath_->setPlaceholderText(detected.isEmpty() ? QStringLiteral("~/dev/ketplus") : detected);
    layout->addWidget(settings_rows::field(
        tr("Allowed origins"),
        tr("Separate origins with commas. Include loopback to allow localhost."), origins_, this));
    layout->addSpacing(14);
    layout->addWidget(settings_rows::field(
        tr("Source checkout path"), tr("Only needed for a source checkout"), appPath_, this));
    layout->addSpacing(20);

    layout->addWidget(textLabel(
        tr("Every browser session uses a temporary profile: it never sees your own browser's "
           "cookies or sign-ins, and everything is discarded when the session ends. Agents run "
           "browser actions without asking first; anything you sign in to inside an agent "
           "session is usable by the agent for that session."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addStretch(1);

    connect(enabled_, &QAbstractButton::toggled, this, &SettingsBrowserPage::setEnabledByUser);
    connect(allOrigins_, &QAbstractButton::toggled, this,
            &SettingsBrowserPage::setAllOriginsByUser);
    connect(origins_, &QLineEdit::editingFinished, this, &SettingsBrowserPage::saveOriginsByUser);
    connect(appPath_, &QLineEdit::editingFinished, this, [this] {
        saveAppPathByUser();
        refresh();
    });
    connect(recheck, &QAbstractButton::clicked, this, &SettingsBrowserPage::refresh);
    updateOriginControls();
    QTimer::singleShot(0, this, &SettingsBrowserPage::refresh);
}

void SettingsBrowserPage::setStatus(const QString& text) {
    status_->setText(text);
    status_->setToolTip(text);
}

void SettingsBrowserPage::updateOriginControls() {
    origins_->setEnabled(onlyOrigins_->isChecked());
}

bool SettingsBrowserPage::saveFields(const std::function<void(browser_tools::Settings&)>& edit) {
    browser_tools::Settings latest = browser_tools::loadSettings(settingsPath_);
    edit(latest);
    if (!browser_tools::saveSettings(latest, settingsPath_)) {
        setStatus(tr("Could not save browser settings."));
        return false;
    }
    settings_ = latest;
    // Running agents pick the change up as soon as they are idle.
    browser_tools::notifySettingsChanged();
    return true;
}

void SettingsBrowserPage::setEnabledByUser(const bool enabled) {
    // Never gated on the runtime: an enabled setting with no runtime simply launches agents
    // without the browser, and turning it off must always work.
    if (!saveFields([enabled](browser_tools::Settings& settings) { settings.enabled = enabled; })) {
        const QSignalBlocker blocker(enabled_);
        enabled_->setChecked(!enabled);
    }
}

void SettingsBrowserPage::setAllOriginsByUser(const bool all) {
    updateOriginControls();
    if (all) {
        (void)saveFields([](browser_tools::Settings& settings) {
            settings.allowedOrigins = QLatin1String(browser_tools::allOrigins);
        });
        return;
    }
    if (origins_->text().trimmed().isEmpty() || isAllOrigins(origins_->text()))
        origins_->setText(QStringLiteral("loopback"));
    saveOriginsByUser();
}

void SettingsBrowserPage::saveOriginsByUser() {
    if (!onlyOrigins_->isChecked())
        return;
    QString error;
    const QString origins = browser_tools::normalizeAllowedOrigins(origins_->text(), &error);
    if (!error.isEmpty()) {
        setStatus(tr("Allowed origins not saved: %1").arg(error));
        return;
    }
    if (isAllOrigins(origins)) {
        allOrigins_->setChecked(true); // "*" in the list means all websites
        return;
    }
    if (origins_->text() != origins)
        origins_->setText(origins);
    if (browser_tools::loadSettings(settingsPath_).allowedOrigins == origins)
        return;
    (void)saveFields(
        [&origins](browser_tools::Settings& settings) { settings.allowedOrigins = origins; });
}

void SettingsBrowserPage::saveAppPathByUser() {
    const QString path = appPath_->text().trimmed();
    if (browser_tools::loadSettings(settingsPath_).browserAppPath == path)
        return;
    (void)saveFields(
        [&path](browser_tools::Settings& settings) { settings.browserAppPath = path; });
}

void SettingsBrowserPage::refresh() {
    settings_.browserAppPath = appPath_->text().trimmed();
    const quint64 generation = ++generation_;
    checking_ = true;
    const auto cached = browser_tools::loadPrerequisiteCache(settingsPath_);
    if (cached && cached->key == browser_tools::prerequisiteCacheKey(settings_, searchStart_)) {
        // Revalidated below; the source is re-derived so a bundled runtime is not shown as the
        // source checkout the cached check described.
        applyCheck({cached->result, runtimeSourceFor(cached->result, settings_, searchStart_)});
    } else {
        prerequisites_ = {};
        runtimeSource_.clear();
        setStatus(tr("Checking the browser runtime…"));
    }
    auto* watcher = new QFutureWatcher<Check>(this);
    const QPointer<SettingsBrowserPage> guard(this);
    connect(watcher, &QFutureWatcher<Check>::finished, this, [this, guard, watcher, generation] {
        watcher->deleteLater();
        if (!guard || generation != generation_)
            return; // a newer refresh (path or settings change) owns the status
        checking_ = false;
        const Check result = watcher->result();
        applyCheck(result);
        (void)browser_tools::savePrerequisiteCache(
            browser_tools::prerequisiteCacheKey(settings_, searchStart_), result.prerequisites,
            settingsPath_);
        emit prerequisitesChecked();
    });
    // The worker owns copies only: the page may be destroyed while node probes run.
    watcher->setFuture(
        QtConcurrent::run([settings = settings_, searchStart = searchStart_, probes = probes_] {
            Check check;
            check.prerequisites = browser_tools::detectPrerequisites(settings, searchStart, probes);
            check.source = runtimeSourceFor(check.prerequisites, settings, searchStart);
            return check;
        }));
}

void SettingsBrowserPage::applyCheck(const Check& check) {
    prerequisites_ = check.prerequisites;
    runtimeSource_ = check.source;
    const QString loadError = browser_tools::loadSettings(settingsPath_).loadError;
    if (!loadError.isEmpty()) {
        // The damaged file is not rewritten by a refresh (no node path save): it stays off
        // until the user saves a choice.
        setStatus(tr("Browser settings could not be read: %1").arg(loadError));
        return;
    }
    if (runtimeSource_ == QLatin1String(browser_tools::SourceCheckoutProvider::sourceName)) {
        setStatus(tr("Ready · source checkout · Node %1 · %2")
                      .arg(prerequisites_.nodeVersion, prerequisites_.browserAppPath));
    } else if (!runtimeSource_.isEmpty()) {
        setStatus(tr("Ready · %1 browser runtime").arg(runtimeSource_));
    } else {
        setStatus(tr("Browser unavailable: %1")
                      .arg(prerequisites_.reason.isEmpty() ? tr("no browser runtime was found.")
                                                           : prerequisites_.reason));
    }
    if (prerequisites_.ok()) {
        // Only the detected Node path is written on refresh (the launch reads it); user fields
        // (enabled, origins, app path) are saved solely from their own edits.
        const QString node = prerequisites_.nodePath;
        if (browser_tools::loadSettings(settingsPath_).nodePath != node)
            (void)saveFields(
                [&node](browser_tools::Settings& settings) { settings.nodePath = node; });
    }
}

} // namespace ketplus
