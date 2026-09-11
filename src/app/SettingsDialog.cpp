#include "app/SettingsDialog.h"

#include "ui/Theme.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace ketplus {

namespace {

QLabel* makeLabel(const QString& text, const QString& role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("kvRole", role);
    label->setWordWrap(true);
    return label;
}

QWidget* makePageHeading(const QString& eyebrow, const QString& title, const QString& description,
                         QWidget* parent) {
    auto* heading = new QWidget(parent);
    auto* layout = new QVBoxLayout(heading);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    layout->addWidget(makeLabel(eyebrow, QStringLiteral("settingsEyebrow"), heading));
    layout->addWidget(makeLabel(title, QStringLiteral("settingsTitle"), heading));
    layout->addWidget(makeLabel(description, QStringLiteral("settingsDescription"), heading));
    return heading;
}

} // namespace

SettingsPage::SettingsPage(QString id, QString title, QWidget* parent)
    : QWidget(parent), id_(std::move(id)), title_(std::move(title)) {}

const QString& SettingsPage::id() const noexcept { return id_; }

const QString& SettingsPage::title() const noexcept { return title_; }

bool SettingsPage::hasChanges() const { return false; }

void SettingsPage::resetToDefaults() {}

void SettingsPage::apply() {}

SettingsDialog::SettingsDialog(const AppearanceSettings& settings, ThemeManager& theme,
                               QWidget* parent)
    : QDialog(parent), theme_(&theme), initialThemeMode_(static_cast<int>(theme.mode())),
      selectedThemeMode_(initialThemeMode_) {
    initialize(settings);
}

SettingsDialog::SettingsDialog(const EditorSettings& settings, QWidget* parent) : QDialog(parent) {
    auto appearance = AppearanceSettings::defaults();
    appearance.editor = settings;
    initialize(appearance);
}

void SettingsDialog::initialize(const AppearanceSettings& settings) {
    initialSettings_ = settings.normalized();
    setWindowTitle(QStringLiteral("Settings"));
    setProperty("kvRole", QStringLiteral("settingsDialog"));
    setModal(true);
    resize(920, 650);
    setMinimumSize(760, 560);

    auto* header = new QWidget(this);
    header->setProperty("kvRole", QStringLiteral("settingsHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 0, 14, 0);
    headerLayout->addWidget(
        makeLabel(QStringLiteral("Settings"), QStringLiteral("settingsHeaderTitle"), header));
    headerLayout->addStretch(1);

    navigation_ = new QListWidget(this);
    navigation_->setObjectName(QStringLiteral("settingsNavigation"));
    navigation_->setProperty("kvRole", QStringLiteral("settingsNavigation"));
    navigation_->setFixedWidth(196);
    navigation_->setSpacing(2);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("settingsPages"));
    addNavigationPage(QStringLiteral("general"), QStringLiteral("General"), createGeneralPage());
    addNavigationPage(QStringLiteral("appearance"), QStringLiteral("Appearance"),
                      createAppearancePage());

    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(navigation_);
    bodyLayout->addWidget(pages_, 1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    resetButton_ =
        buttons_->addButton(QStringLiteral("Reset defaults"), QDialogButtonBox::ResetRole);
    resetButton_->setProperty("kvRole", QStringLiteral("settingsReset"));
    saveButton_ = buttons_->addButton(QStringLiteral("Save changes"), QDialogButtonBox::AcceptRole);
    saveButton_->setProperty("kvRole", QStringLiteral("primaryAction"));
    saveButton_->setDefault(true);

    auto* footer = new QWidget(this);
    footer->setProperty("kvRole", QStringLiteral("settingsFooter"));
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(14, 9, 14, 9);
    footerLayout->addWidget(buttons_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(body, 1);
    layout->addWidget(footer);

    connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
    connect(buttons_, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(resetButton_, &QPushButton::clicked, this, [this] {
        setAppearanceSettings(AppearanceSettings::defaults());
        selectThemeMode(static_cast<int>(ThemeManager::Mode::System));
        for (SettingsPage* page : std::as_const(extensionPages_)) {
            page->resetToDefaults();
        }
        updateDirtyState();
    });
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        const auto appearance = appearanceSettings();
        for (SettingsPage* page : std::as_const(extensionPages_)) {
            page->apply();
        }
        if (theme_ != nullptr) {
            theme_->setMode(static_cast<ThemeManager::Mode>(selectedThemeMode_));
        }
        emit appearanceSettingsSaved(appearance);
        emit settingsSaved(appearance.editor);
        accept();
    });

    setAppearanceSettings(initialSettings_);
    navigation_->setCurrentRow(1);
    updateThemePreview();
    updateDirtyState();
}

QWidget* SettingsDialog::createGeneralPage() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 26, 30, 30);
    layout->setSpacing(18);
    layout->addWidget(makePageHeading(
        QStringLiteral("GENERAL"), QStringLiteral("Application preferences"),
        QStringLiteral("Shared startup, workspace and update settings will appear here."), page));
    auto* card = new QFrame(page);
    card->setProperty("kvRole", QStringLiteral("settingsCard"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 20, 18, 20);
    cardLayout->addWidget(makeLabel(QStringLiteral("No general preferences are configurable yet."),
                                    QStringLiteral("settingsDescription"), card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

void SettingsDialog::addNavigationPage(const QString& id, const QString& title, QWidget* page) {
    auto* item = new QListWidgetItem(title, navigation_);
    item->setData(Qt::UserRole, id);
    pages_->addWidget(page);
}

void SettingsDialog::addPage(SettingsPage* page) {
    if (page == nullptr) {
        return;
    }
    extensionPages_.append(page);
    addNavigationPage(page->id(), page->title(), page);
    connect(page, &SettingsPage::changed, this, &SettingsDialog::updateDirtyState);
    updateDirtyState();
}

AppearanceSettings SettingsDialog::appearanceSettings() const {
    return AppearanceSettings{
        .interfaceFontSizePixels = interfaceFontSizeBox_->value(),
        .editor =
            EditorSettings{
                .fontFamily = editorFontBox_->currentFont().family(),
                .fontSizePixels = editorFontSizeBox_->value(),
                .lineHeightPixels = editorLineHeightBox_->value(),
            },
        .terminal =
            TextTypographySettings{terminalFontSizeBox_->value(), terminalLineHeightBox_->value()},
        .preview =
            TextTypographySettings{previewFontSizeBox_->value(), previewLineHeightBox_->value()},
    }
        .normalized();
}

EditorSettings SettingsDialog::settings() const { return appearanceSettings().editor; }

void SettingsDialog::setAppearanceSettings(const AppearanceSettings& settings) {
    const auto value = settings.normalized();
    editorFontBox_->setCurrentFont(QFont(value.editor.fontFamily));
    interfaceFontSizeBox_->setValue(value.interfaceFontSizePixels);
    editorFontSizeBox_->setValue(value.editor.fontSizePixels);
    editorLineHeightBox_->setMinimum(
        qMax(EditorSettings::minimumLineHeightPixels, value.editor.fontSizePixels));
    editorLineHeightBox_->setValue(value.editor.lineHeightPixels);
    terminalFontSizeBox_->setValue(value.terminal.fontSizePixels);
    terminalLineHeightBox_->setMinimum(
        qMax(TextTypographySettings::minimumLineHeightPixels, value.terminal.fontSizePixels));
    terminalLineHeightBox_->setValue(value.terminal.lineHeightPixels);
    previewFontSizeBox_->setValue(value.preview.fontSizePixels);
    previewLineHeightBox_->setMinimum(
        qMax(TextTypographySettings::minimumLineHeightPixels, value.preview.fontSizePixels));
    previewLineHeightBox_->setValue(value.preview.lineHeightPixels);
    updatePreview();
}

void SettingsDialog::updateDirtyState() {
    if (saveButton_ == nullptr) {
        return;
    }
    bool extensionChanged = false;
    for (const SettingsPage* page : std::as_const(extensionPages_)) {
        extensionChanged = extensionChanged || page->hasChanges();
    }
    const bool changed = appearanceSettings() != initialSettings_ ||
                         selectedThemeMode_ != initialThemeMode_ || extensionChanged;
    saveButton_->setEnabled(changed);
}

void SettingsDialog::updateThemePreview() {
    const bool followsSystem = selectedThemeMode_ == static_cast<int>(ThemeManager::Mode::System);
    {
        const QSignalBlocker blocker(followSystem_);
        followSystem_->setChecked(followsSystem);
    }
    themeList_->setEnabled(!followsSystem && theme_ != nullptr);
    for (int index = 0; index < themeList_->count(); ++index) {
        auto* item = themeList_->item(index);
        if (item->data(Qt::UserRole).toInt() == selectedThemeMode_) {
            const QSignalBlocker blocker(themeList_);
            themeList_->setCurrentItem(item);
            break;
        }
    }
    if (theme_ != nullptr) {
        theme_->previewMode(static_cast<ThemeManager::Mode>(selectedThemeMode_));
    } else {
        followSystem_->setEnabled(false);
        themeList_->setEnabled(false);
    }
}

void SettingsDialog::selectThemeMode(const int mode) {
    selectedThemeMode_ = mode;
    updateThemePreview();
    updateDirtyState();
}

void SettingsDialog::reject() {
    if (theme_ != nullptr) {
        theme_->cancelPreview();
    }
    QDialog::reject();
}

} // namespace ketplus
