#include "app/SettingsUpdatesPage.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetToggle.h"
#include "KetUpdateCard.h"

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
    layout->addSpacing(14);
    intro_ = textLabel({}, QStringLiteral("updatesStatus"), this);
    layout->addWidget(intro_);
    layout->addSpacing(14);

    card_ = new ket::UpdateCard({}, this);
    card_->setObjectName(QStringLiteral("updatesCard"));
    badge_ = new ket::Badge({}, ket::BadgeTone::Neutral);
    card_->addBadge(badge_);
    restart_ = new ket::Button(tr("Restart now"), ket::ButtonVariant::Primary);
    restart_->setObjectName(QStringLiteral("updatesRestart"));
    card_->addAction(restart_);
    later_ = new ket::Button(tr("Later"), ket::ButtonVariant::Ghost);
    later_->setObjectName(QStringLiteral("updatesLater"));
    card_->addAction(later_);
    // No release notes here: Sparkle's window shows them.
    layout->addWidget(card_);
    layout->addSpacing(14);

    automaticChecks_ = new ket::Checkbox(tr("Check for updates automatically"), this);
    automaticChecks_->setObjectName(QStringLiteral("updatesAutomaticChecks"));
    layout->addWidget(automaticChecks_);
    layout->addSpacing(8);
    automaticDownloads_ = new ket::Checkbox(tr("Download updates automatically"), this);
    automaticDownloads_->setObjectName(QStringLiteral("updatesAutomaticDownloads"));
    layout->addWidget(automaticDownloads_);
    layout->addSpacing(14);
    auto* check = new ket::Button(tr("Check for updates"), ket::ButtonVariant::Secondary, this);
    check->setIcon(ket::Icons::icon(u"refresh"));
    check->setObjectName(QStringLiteral("updatesCheckNow"));
    check_ = check;
    layout->addWidget(check_, 0, Qt::AlignLeft);
    layout->addStretch(1);

    // Sparkle's prompt for a downloaded update installs it and relaunches; turns keep running
    // in the daemon, which the new Desktop replaces once it is idle (ADR 0003).
    connect(restart_, &QAbstractButton::clicked, this, [this] { updater_.checkForUpdates(); });
    connect(later_, &QAbstractButton::clicked, this, [this] {
        laterVersion_ = updater_.pendingVersion();
        refresh();
    });
    connect(check_, &QAbstractButton::clicked, this, [this] { updater_.checkForUpdates(); });
    // Downloading in the background needs the scheduled checks: turning checks off turns
    // downloads off, and turning downloads on turns checks on.
    connect(automaticChecks_, &QAbstractButton::toggled, this, [this](const bool enabled) {
        if (!enabled)
            updater_.setAutomaticallyDownloadsUpdates(false);
        updater_.setAutomaticallyChecksForUpdates(enabled);
        refresh();
    });
    connect(automaticDownloads_, &QAbstractButton::toggled, this, [this](const bool enabled) {
        if (enabled)
            updater_.setAutomaticallyChecksForUpdates(true);
        updater_.setAutomaticallyDownloadsUpdates(enabled);
        refresh();
    });
    connect(&updater_, &AppUpdater::stateChanged, this, &SettingsUpdatesPage::refresh);
    connect(&updater_, &AppUpdater::pendingVersionChanged, this, &SettingsUpdatesPage::refresh);
    refresh();
}

void SettingsUpdatesPage::refresh() {
    const bool configured = updater_.isConfigured();
    const bool checks = configured && updater_.automaticallyChecksForUpdates();
    if (!configured)
        intro_->setText(tr("Updates are not configured for this build. Install new versions "
                           "manually; KetPlus does not contact an update server."));
    else if (checks)
        intro_->setText(tr("KetPlus checks for updates every day and installs them when you "
                           "restart."));
    else
        intro_->setText(tr("KetPlus checks for updates when you ask and installs them when you "
                           "restart."));

    const QString pending = updater_.pendingVersion();
    const bool downloaded = !pending.isEmpty() && updater_.pendingVersionDownloaded();
    card_->setVisible(configured);
    card_->setReady(downloaded);
    if (downloaded) {
        card_->setTitle(tr("KétPlus Pro %1").arg(pending));
        badge_->setText(tr("Ready"));
        badge_->setTone(ket::BadgeTone::Success);
        card_->setDetail(laterVersion_ == pending
                             ? tr("Downloaded and verified. It installs the next time KetPlus "
                                  "restarts.")
                             : tr("Downloaded and verified. Restart to finish installing; "
                                  "running turns finish first."));
    } else if (!pending.isEmpty()) {
        card_->setTitle(tr("KétPlus Pro %1").arg(pending));
        badge_->setText(tr("Available"));
        badge_->setTone(ket::BadgeTone::Accent);
        card_->setDetail(tr("Check for updates to review and install it."));
    } else {
        card_->setTitle(tr("KétPlus Pro %1").arg(QCoreApplication::applicationVersion()));
        const QDateTime checked = updater_.lastUpdateCheck();
        badge_->setText(checked.isValid() ? tr("Up to date") : tr("Installed"));
        badge_->setTone(ket::BadgeTone::Neutral);
        card_->setDetail(checked.isValid() ? tr("Checked %1.")
                                                 .arg(QLocale().toString(checked.toLocalTime(),
                                                                         QLocale::ShortFormat))
                                           : tr("KetPlus has not checked for updates yet."));
    }
    // UpdateCard shows its action row once it has an action; hide the row with its buttons.
    restart_->parentWidget()->setVisible(downloaded && laterVersion_ != pending);
    restart_->setEnabled(updater_.canCheckForUpdates());

    check_->setEnabled(configured && updater_.canCheckForUpdates());
    {
        const QSignalBlocker block(automaticDownloads_);
        const QSignalBlocker blockChecks(automaticChecks_);
        automaticChecks_->setChecked(checks);
        automaticDownloads_->setChecked(checks && updater_.automaticallyDownloadsUpdates());
    }
    automaticChecks_->setEnabled(configured);
    automaticDownloads_->setEnabled(configured);
}

} // namespace ketplus
