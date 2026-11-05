#include "app/SettingsComputerPage.h"

#include "ComputerToolLaunch.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    return value;
}

QStringList parseBundleIds(const QString& text) {
    QStringList ids;
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString id = part.trimmed();
        if (!id.isEmpty() && !ids.contains(id))
            ids.append(id);
    }
    return ids;
}

} // namespace

SettingsComputerPage::SettingsComputerPage(QString settingsPath, QString applicationDir,
                                           QWidget* parent)
    : QWidget(parent), settingsPath_(settingsPath.isEmpty() ? computer_tools::settingsFilePath()
                                                            : std::move(settingsPath)),
      applicationDir_(applicationDir.isEmpty() ? QCoreApplication::applicationDirPath()
                                               : std::move(applicationDir)) {
    setObjectName(QStringLiteral("settingsWorkspaceComputer"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Computer use"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(7);
    layout->addWidget(textLabel(
        tr("Let agents see and control native Mac apps through KetPlus Computer, for tasks no "
           "command or the KetPlus browser can do."),
        QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(24);

    status_ = textLabel({}, QStringLiteral("computerStatus"), this);
    layout->addWidget(status_);
    layout->addSpacing(10);
    enabled_ = new QCheckBox(tr("Allow agents to use this computer"), this);
    enabled_->setObjectName(QStringLiteral("computerEnabled"));
    layout->addWidget(enabled_);
    layout->addSpacing(4);
    layout->addWidget(textLabel(
        tr("Applies from each conversation's next turn. Agents still ask you to approve every app "
           "in every conversation."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addSpacing(24);

    layout->addWidget(
        textLabel(tr("Work display"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    display_ = new QComboBox(this);
    display_->setObjectName(QStringLiteral("computerWorkDisplay"));
    display_->setAccessibleName(tr("Work display"));
    display_->addItem(tr("Secondary display"), QStringLiteral("secondary"));
    display_->addItem(tr("Main display"), QStringLiteral("main"));
    layout->addWidget(display_, 0, Qt::AlignLeft);
    layout->addSpacing(4);
    layout->addWidget(textLabel(
        tr("The agent works on this display: approved apps move there and screenshots show it. "
           "With a single display, it uses the main one."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addSpacing(24);

    layout->addWidget(
        textLabel(tr("Blocked apps"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    deniedApps_ = new QLineEdit(this);
    deniedApps_->setObjectName(QStringLiteral("computerDeniedApps"));
    deniedApps_->setPlaceholderText(QStringLiteral("com.example.Banking, com.example.Mail"));
    deniedApps_->setAccessibleName(tr("Blocked app bundle IDs"));
    layout->addWidget(deniedApps_);
    layout->addSpacing(4);
    layout->addWidget(textLabel(
        tr("Bundle IDs separated by commas. KetPlus, Keychain Access and password managers are "
           "always blocked. Terminals and IDEs accept clicks only, and browsers other than Safari "
           "are view-only."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    error_ = textLabel({}, QStringLiteral("computerSettingsError"), this);
    error_->setVisible(false);
    layout->addWidget(error_);
    layout->addSpacing(24);

    layout->addWidget(textLabel(tr("Safety"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    layout->addWidget(textLabel(
        tr("Screenshots show only the apps you approved; other apps and KetPlus stay hidden. "
           "Agents work in the background without taking your pointer. Press Stop on the banner, "
           "or Esc in an approved app, to stop: the agent loses its approvals and must ask again. "
           "Turns started from your phone cannot control this Mac."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addSpacing(24);

    layout->addWidget(
        textLabel(tr("macOS permissions"), QStringLiteral("settingsWorkspaceSubheading"), this));
    layout->addSpacing(10);
    layout->addWidget(textLabel(
        tr("KetPlus Computer asks for Screen Recording and Accessibility the first time an agent "
           "needs them. Grant both to KetPlus Computer, not to KetPlus."),
        QStringLiteral("settingsWorkspaceHelp"), this));
    layout->addSpacing(8);
    auto* privacy = new QPushButton(tr("Open Privacy & Security"), this);
    privacy->setObjectName(QStringLiteral("computerOpenPrivacy"));
    layout->addWidget(privacy, 0, Qt::AlignLeft);
    layout->addStretch(1);

    connect(enabled_, &QCheckBox::toggled, this, &SettingsComputerPage::saveEnabled);
    connect(deniedApps_, &QLineEdit::editingFinished, this, &SettingsComputerPage::saveDeniedApps);
    connect(display_, &QComboBox::currentIndexChanged, this, &SettingsComputerPage::saveDisplay);
    connect(privacy, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")));
    });
    load();
}

void SettingsComputerPage::load() {
    const auto settings = computer_tools::loadSettings(settingsPath_);
    const bool installed = !computer_tools::findFrontExecutable(applicationDir_).isEmpty();
    status_->setText(installed ? tr("KetPlus Computer is installed with this app.")
                               : tr("KetPlus Computer is not included in this build, so agents "
                                    "cannot use the computer."));
    const QSignalBlocker blockEnabled(enabled_);
    enabled_->setChecked(settings.enabled);
    enabled_->setEnabled(installed || settings.enabled);
    deniedApps_->setText(settings.deniedApps.join(QStringLiteral(", ")));
    const QSignalBlocker blockDisplay(display_);
    display_->setCurrentIndex(qMax(0, display_->findData(settings.display)));
}

void SettingsComputerPage::saveEnabled(const bool enabled) {
    auto settings = computer_tools::loadSettings(settingsPath_);
    settings.enabled = enabled;
    const bool saved = computer_tools::saveSettings(settings, settingsPath_);
    error_->setText(saved ? QString() : tr("KetPlus could not save this setting."));
    error_->setVisible(!saved);
    if (!saved) {
        const QSignalBlocker block(enabled_);
        enabled_->setChecked(!enabled);
    }
}

void SettingsComputerPage::saveDeniedApps() {
    auto settings = computer_tools::loadSettings(settingsPath_);
    const QStringList ids = parseBundleIds(deniedApps_->text());
    if (ids == settings.deniedApps)
        return;
    settings.deniedApps = ids;
    const bool saved = computer_tools::saveSettings(settings, settingsPath_);
    error_->setText(saved ? QString() : tr("KetPlus could not save the blocked apps."));
    error_->setVisible(!saved);
    if (saved)
        deniedApps_->setText(ids.join(QStringLiteral(", ")));
}

void SettingsComputerPage::saveDisplay(const int index) {
    auto settings = computer_tools::loadSettings(settingsPath_);
    const QString previous = settings.display;
    settings.display = display_->itemData(index).toString();
    const bool saved = computer_tools::saveSettings(settings, settingsPath_);
    error_->setText(saved ? QString() : tr("KetPlus could not save the work display."));
    error_->setVisible(!saved);
    if (!saved) {
        const QSignalBlocker block(display_);
        display_->setCurrentIndex(qMax(0, display_->findData(previous)));
    }
}

} // namespace ketplus
