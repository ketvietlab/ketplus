#include "app/SettingsUpdatesPage.h"
#include "KetButton.h"
#include "KetToggle.h"

#include "update/AppUpdater.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QLabel>
#include <QLocale>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    return value;
}

} // namespace

SettingsUpdatesPage::SettingsUpdatesPage(AppUpdater& updater, QWidget* parent)
    : QWidget(parent), updater_(updater) {
    setObjectName(QStringLiteral("settingsWorkspaceUpdates"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Updates"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(7);
    layout->addWidget(
        textLabel(tr("KetPlus Desktop %1").arg(QCoreApplication::applicationVersion()),
                  QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(24);

    status_ = textLabel({}, QStringLiteral("updatesStatus"), this);
    layout->addWidget(status_);
    layout->addSpacing(8);
    check_ = new ket::Button(tr("Check for Updates"), ket::ButtonVariant::Secondary, this);
    check_->setObjectName(QStringLiteral("updatesCheckNow"));
    layout->addWidget(check_, 0, Qt::AlignLeft);
    layout->addSpacing(24);

    layout->addWidget(
        textLabel(tr("Preferences"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    automaticChecks_ = new ket::Checkbox(tr("Check for updates automatically"), this);
    automaticChecks_->setObjectName(QStringLiteral("updatesAutomaticChecks"));
    layout->addWidget(automaticChecks_);
    layout->addSpacing(6);
    automaticDownloads_ = new ket::Checkbox(tr("Download updates in the background"), this);
    automaticDownloads_->setObjectName(QStringLiteral("updatesAutomaticDownloads"));
    layout->addWidget(automaticDownloads_);
    layout->addSpacing(4);
    layout->addWidget(textLabel(tr("You still choose when KetPlus restarts. Every update is "
                                   "verified with the KetPlus signing key before it installs."),
                                QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addSpacing(24);

    layout->addWidget(
        textLabel(tr("Running agents"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    layout->addWidget(textLabel(
        tr("Agents run in the KetPlus background service, not in this window. Restarting to "
           "update never stops a turn: the current service finishes its turns, then the new "
           "version takes over."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addStretch(1);

    connect(check_, &QAbstractButton::clicked, this, [this] { updater_.checkForUpdates(); });
    connect(automaticChecks_, &QAbstractButton::toggled, this,
            [this](const bool enabled) { updater_.setAutomaticallyChecksForUpdates(enabled); });
    connect(automaticDownloads_, &QAbstractButton::toggled, this,
            [this](const bool enabled) { updater_.setAutomaticallyDownloadsUpdates(enabled); });
    connect(&updater_, &AppUpdater::stateChanged, this, &SettingsUpdatesPage::refresh);
    connect(&updater_, &AppUpdater::pendingVersionChanged, this, &SettingsUpdatesPage::refresh);
    refresh();
}

void SettingsUpdatesPage::refresh() {
    const bool configured = updater_.isConfigured();
    if (!configured) {
        status_->setText(tr("Updates are not configured for this build. Install new versions "
                            "manually; KetPlus does not contact an update server."));
    } else if (!updater_.pendingVersion().isEmpty()) {
        status_->setText(tr("KetPlus %1 is available. Check for Updates to review and install it.")
                             .arg(updater_.pendingVersion()));
    } else if (const QDateTime checked = updater_.lastUpdateCheck(); checked.isValid()) {
        status_->setText(tr("Last checked %1.")
                             .arg(QLocale().toString(checked.toLocalTime(), QLocale::ShortFormat)));
    } else {
        status_->setText(tr("KetPlus has not checked for updates yet."));
    }
    check_->setEnabled(configured && updater_.canCheckForUpdates());
    {
        const QSignalBlocker blockChecks(automaticChecks_);
        const QSignalBlocker blockDownloads(automaticDownloads_);
        automaticChecks_->setChecked(configured && updater_.automaticallyChecksForUpdates());
        automaticDownloads_->setChecked(configured && updater_.automaticallyDownloadsUpdates());
    }
    automaticChecks_->setEnabled(configured);
    automaticDownloads_->setEnabled(configured && automaticChecks_->isChecked());
}

} // namespace ketplus
