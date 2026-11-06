#include "app/SettingsBrowserPage.h"

#include "ToolPackageRegistry.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
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
    layout->addSpacing(7);
    layout->addWidget(textLabel(tr("Agents can open websites in a browser to check what they "
                                   "build, fill in forms and capture screenshots."),
                                QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(24);

    status_ =
        textLabel(tr("Checking the browser runtime…"), QStringLiteral("browserToolsStatus"), this);
    if (!settings_.loadError.isEmpty())
        status_->setText(tr("Browser settings could not be read: %1").arg(settings_.loadError));
    layout->addWidget(status_);
    layout->addSpacing(8);
    auto* recheck = new QPushButton(tr("Check again"), this);
    recheck->setObjectName(QStringLiteral("browserToolsRecheck"));
    layout->addWidget(recheck, 0, Qt::AlignLeft);
    layout->addSpacing(20);

    enabled_ = new QCheckBox(tr("Let agents use a browser"), this);
    enabled_->setObjectName(QStringLiteral("browserToolsEnabled"));
    enabled_->setChecked(settings_.enabled);
    layout->addWidget(enabled_);
    layout->addSpacing(16);

    layout->addWidget(
        textLabel(tr("Websites"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    allOrigins_ = new QRadioButton(tr("Public websites and this computer"), this);
    allOrigins_->setObjectName(QStringLiteral("browserToolsAllOrigins"));
    onlyOrigins_ = new QRadioButton(tr("Only these origins"), this);
    onlyOrigins_->setObjectName(QStringLiteral("browserToolsOnlyOrigins"));
    auto* originMode = new QButtonGroup(this);
    originMode->addButton(allOrigins_);
    originMode->addButton(onlyOrigins_);
    (isAllOrigins(settings_.allowedOrigins) ? allOrigins_ : onlyOrigins_)->setChecked(true);
    layout->addWidget(allOrigins_);
    layout->addSpacing(6);
    layout->addWidget(onlyOrigins_);
    layout->addSpacing(8);

    auto* fields = new QWidget(this);
    auto* grid = new QGridLayout(fields);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(20);
    grid->setVerticalSpacing(13);
    grid->setColumnMinimumWidth(0, 145);
    grid->setColumnStretch(1, 1);
    origins_ = new QLineEdit(
        isAllOrigins(settings_.allowedOrigins) ? QString() : settings_.allowedOrigins, fields);
    origins_->setObjectName(QStringLiteral("browserToolsOrigins"));
    origins_->setPlaceholderText(QStringLiteral("loopback, https://staging.example.com"));
    appPath_ = new QLineEdit(settings_.browserAppPath, fields);
    appPath_->setObjectName(QStringLiteral("browserToolsAppPath"));
    const QString detected = browser_tools::detectBrowserAppPath(searchStart_);
    appPath_->setPlaceholderText(detected.isEmpty() ? tr("Only needed for a source checkout")
                                                    : detected);
    grid->addWidget(
        textLabel(tr("Allowed origins"), QStringLiteral("settingsWorkspaceFieldLabel"), fields), 0,
        0);
    grid->addWidget(origins_, 0, 1);
    grid->addWidget(textLabel(tr("Source checkout path"),
                              QStringLiteral("settingsWorkspaceFieldLabel"), fields),
                    1, 0);
    grid->addWidget(appPath_, 1, 1);
    layout->addWidget(fields);
    layout->addSpacing(12);

    layout->addWidget(textLabel(
        tr("Every browser session uses a temporary profile: it never sees your own browser's "
           "cookies or sign-ins, and everything is discarded when the session ends. Agents run "
           "browser actions without asking first; anything you sign in to inside an agent "
           "session is usable by the agent for that session."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addStretch(1);

    connect(enabled_, &QCheckBox::toggled, this, &SettingsBrowserPage::setEnabledByUser);
    connect(allOrigins_, &QRadioButton::toggled, this, &SettingsBrowserPage::setAllOriginsByUser);
    connect(origins_, &QLineEdit::editingFinished, this, &SettingsBrowserPage::saveOriginsByUser);
    connect(appPath_, &QLineEdit::editingFinished, this, [this] {
        saveAppPathByUser();
        refresh();
    });
    connect(recheck, &QPushButton::clicked, this, &SettingsBrowserPage::refresh);
    updateOriginControls();
    QTimer::singleShot(0, this, &SettingsBrowserPage::refresh);
}

void SettingsBrowserPage::updateOriginControls() {
    origins_->setEnabled(onlyOrigins_->isChecked());
}

bool SettingsBrowserPage::saveFields(const std::function<void(browser_tools::Settings&)>& edit) {
    browser_tools::Settings latest = browser_tools::loadSettings(settingsPath_);
    edit(latest);
    if (!browser_tools::saveSettings(latest, settingsPath_)) {
        status_->setText(tr("Could not save browser settings."));
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
        status_->setText(tr("Allowed origins not saved: %1").arg(error));
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
        status_->setText(tr("Checking the browser runtime…"));
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
        status_->setText(tr("Browser settings could not be read: %1").arg(loadError));
        return;
    }
    if (runtimeSource_ == QLatin1String(browser_tools::SourceCheckoutProvider::sourceName)) {
        status_->setText(tr("Ready · source checkout · Node %1 · %2")
                             .arg(prerequisites_.nodeVersion, prerequisites_.browserAppPath));
    } else if (!runtimeSource_.isEmpty()) {
        status_->setText(tr("Ready · %1 browser runtime").arg(runtimeSource_));
    } else {
        status_->setText(tr("Browser unavailable: %1")
                             .arg(prerequisites_.reason.isEmpty()
                                      ? tr("no browser runtime was found.")
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
