#include "app/SettingsDialog.h"

#include "ui/InterfaceZoom.h"

#include "ui/Theme.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetNavItem.h"
#include "KetSegmentedControl.h"
#include "KetThemeCard.h"
#include "app/PixelSelect.h"
#include "app/SettingsFontNames.h"
#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace ketplus {

namespace {

QLabel* makeLabel(const QString& text, const QString& role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("kvRole", role);
    label->setWordWrap(true);
    return label;
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

SettingsDialog::~SettingsDialog() {
    // A scheme panel cancels its preview while QWidget deletes the children. By
    // then our control lists have already been destroyed, so stop theme callbacks now.
    if (theme_)
        disconnect(theme_, nullptr, this, nullptr);
    for (SettingsPage* page : std::as_const(extensionPages_)) {
        disconnect(page, nullptr, this, nullptr);
    }
    registeredPages_.clear();
    extensionPages_.clear();
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
    addNavigationPage(QStringLiteral("appearance"), QStringLiteral("Appearance"),
                      createAppearancePage());
    addNavigationPage(QStringLiteral("typography"), tr("Typography"), createTypographyCard());

    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(navigation_);
    bodyLayout->addWidget(pages_, 1);

    buttons_ = new QWidget(this);
    buttons_->setObjectName(QStringLiteral("settingsButtons"));
    auto* actions = new QHBoxLayout(buttons_);
    actions->setContentsMargins(0, 0, 0, 0);
    // Every change applies as it is made (Jay, 2026-11-10), so the row holds only the reset.
    resetButton_ = new ket::Button(tr("Restore defaults"), ket::ButtonVariant::Ghost, buttons_);
    resetButton_->setProperty("kvRole", QStringLiteral("settingsReset"));
    resetButton_->setObjectName(QStringLiteral("settingsResetButton"));
    actions->addWidget(resetButton_);
    actions->addStretch();

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
    connect(resetButton_, &QAbstractButton::clicked, this, [this] {
        setAppearanceSettings(AppearanceSettings::defaults());
        if (theme_)
            (void)theme_->setSchemeSelection(ThemeSelection{});
        selectedThemeMode_ = static_cast<int>(theme_ ? theme_->mode() : ThemeManager::Mode::System);
        updateThemeCards();
        for (SettingsPage* page : std::as_const(extensionPages_)) {
            page->resetToDefaults();
        }
        applyChanges();
    });
    setAppearanceSettings(initialSettings_);
    navigation_->setCurrentRow(0);
    // What the controls show is applied already; later edits apply as they are made.
    appliedSettings_ = appearanceSettings();
    ready_ = true;
    updateThemeCards();
    // The system, or another window, can change the theme while this dialog is open; the
    // selection and its marks follow the mode in use.
    if (theme_ != nullptr)
        connect(theme_, &ThemeManager::themeChanged, this, [this] {
            selectedThemeMode_ = static_cast<int>(theme_->mode());
            updateThemeCards();
        });
}

void SettingsDialog::addNavigationPage(const QString& id, const QString& title, QWidget* page) {
    auto* item = new QListWidgetItem(title, navigation_);
    item->setData(Qt::UserRole, id);
    auto* row = new ket::NavItem(
        title,
        ket::Icons::codeForLucide(id == QStringLiteral("appearance") ? u"palette" : u"settings"),
        navigation_);
    item->setSizeHint(row->sizeHint());
    navigation_->setItemWidget(item, row);
    const QPersistentModelIndex index(navigation_->indexFromItem(item));
    const QPointer<QListWidget> navigation(navigation_);
    connect(row, &QAbstractButton::clicked, navigation_, [navigation, index] {
        if (navigation && index.isValid())
            navigation->setCurrentIndex(index);
    });
    connect(navigation_, &QListWidget::currentItemChanged, row, [navigation, row, index] {
        row->setChecked(navigation && index.isValid() && navigation->currentIndex() == index);
    });
    // QListWidget deletes items immediately but defers deleting their widgets. A zoom
    // signal can arrive in that interval while Pro rebuilds its extension pages.
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, row,
            [navigation, row, index] {
                if (navigation && index.isValid())
                    navigation->item(index.row())->setSizeHint(row->sizeHint());
            });
    pages_->addWidget(page);
}

void SettingsDialog::addPage(SettingsPage* page) {
    if (page == nullptr) {
        return;
    }
    registerPage({SettingsPageDescriptor{page->id(), page->title(), {}, 0}, page});
}

SettingsRegistrationResult SettingsDialog::registerPage(SettingsPageRegistration registration) {
    SettingsPage* page = registration.page;
    if (page == nullptr) {
        return SettingsRegistrationResult::MissingPage;
    }
    if (std::any_of(registeredPages_.cbegin(), registeredPages_.cend(),
                    [page](const RegisteredPage& value) { return value.page == page; })) {
        return SettingsRegistrationResult::PointerAlreadyRegistered;
    }
    if (page->parent() != nullptr) {
        return SettingsRegistrationResult::AlreadyParented;
    }

    static const QRegularExpression validId(
        QStringLiteral("^[a-z0-9][a-z0-9_-]*(/[a-z0-9][a-z0-9_-]*)*$"));
    const QString& id = registration.descriptor.id;
    if (!validId.match(id).hasMatch()) {
        return SettingsRegistrationResult::InvalidId;
    }
    if (id == QStringLiteral("general") || id == QStringLiteral("appearance") ||
        id == QStringLiteral("typography") ||
        std::any_of(registeredPages_.cbegin(), registeredPages_.cend(),
                    [&id](const RegisteredPage& value) { return value.descriptor.id == id; })) {
        return SettingsRegistrationResult::DuplicateId;
    }
    if (registration.descriptor.id != page->id() ||
        registration.descriptor.title != page->title()) {
        return SettingsRegistrationResult::MetadataMismatch;
    }

    const QString registeredId = registration.descriptor.id;
    registeredPages_.append(
        RegisteredPage{std::move(registration.descriptor), page, nextRegistrationSequence_++});
    extensionPages_.append(page);
    connect(page, &SettingsPage::changed, this, &SettingsDialog::applyChanges);
    connect(page, &QObject::destroyed, this, [this, page, registeredId] {
        const bool selectedWasDestroyed = selectedPageId() == registeredId;
        extensionPages_.removeAll(page);
        registeredPages_.erase(
            std::remove_if(registeredPages_.begin(), registeredPages_.end(),
                           [page](const RegisteredPage& value) { return value.page == page; }),
            registeredPages_.end());
        rebuildExtensionNavigation();
        if (selectedWasDestroyed && navigation_->count() > 0) {
            navigation_->setCurrentRow(0);
        }
        applyChanges();
    });
    rebuildExtensionNavigation();
    applyChanges();
    return SettingsRegistrationResult::Registered;
}

bool SettingsDialog::selectPage(const QString& id) {
    for (int index = 0; index < navigation_->count(); ++index) {
        if (navigation_->item(index)->data(Qt::UserRole).toString() == id) {
            navigation_->setCurrentRow(index);
            return true;
        }
    }
    return false;
}

QString SettingsDialog::selectedPageId() const {
    const QListWidgetItem* item = navigation_ == nullptr ? nullptr : navigation_->currentItem();
    return item == nullptr ? QString{} : item->data(Qt::UserRole).toString();
}

void SettingsDialog::rebuildExtensionNavigation() {
    const QString selection = selectedPageId();
    while (navigation_->count() > 2) {
        delete navigation_->takeItem(2);
    }
    for (int index = pages_->count() - 1; index >= 2; --index) {
        pages_->removeWidget(pages_->widget(index));
    }

    std::stable_sort(registeredPages_.begin(), registeredPages_.end(),
                     [](const RegisteredPage& left, const RegisteredPage& right) {
                         if (left.descriptor.order != right.descriptor.order) {
                             return left.descriptor.order < right.descriptor.order;
                         }
                         return left.sequence < right.sequence;
                     });
    for (const RegisteredPage& value : std::as_const(registeredPages_)) {
        addNavigationPage(value.descriptor.id, value.descriptor.title, value.page);
    }
    if (!selection.isEmpty() && selectPage(selection)) {
        return;
    }
    if (navigation_->currentRow() < 0 && navigation_->count() > 0) {
        navigation_->setCurrentRow(0);
    }
}

AppearanceSettings SettingsDialog::appearanceSettings() const {
    const auto zoomSteps = InterfaceZoomMetrics::steps();
    return AppearanceSettings{
        .interfaceZoomPercent =
            zoomSteps.value(zoomControl_->currentIndex(), InterfaceZoomMetrics::defaultPercent),
        .interfaceFontSizePixels = interfaceFontSizeBox_->value(),
        .editor =
            EditorSettings{
                .fontFamily = settings_fonts::selectedFamily(editorFontBox_).isEmpty()
                                  ? editorFontFamily_
                                  : settings_fonts::selectedFamily(editorFontBox_),
                .fontSizePixels = editorFontSizeBox_->value(),
                .lineHeightPixels = editorLineHeightBox_->value(),
            },
        .terminalFontFamily = terminalFontFamily_,
    }
        .normalized();
}

EditorSettings SettingsDialog::settings() const { return appearanceSettings().editor; }

void SettingsDialog::setAppearanceSettings(const AppearanceSettings& settings) {
    const auto value = settings.normalized();
    editorFontFamily_ = value.editor.fontFamily;
    terminalFontFamily_ = value.terminalFontFamily;
    settings_fonts::selectFamily(editorFontBox_, editorFontFamily_);
    zoomControl_->setCurrentIndex(
        int(InterfaceZoomMetrics::steps().indexOf(value.interfaceZoomPercent)));
    interfaceFontSizeBox_->setValue(value.interfaceFontSizePixels);
    editorFontSizeBox_->setValue(value.editor.fontSizePixels);
    editorLineHeightBox_->setMinimum(
        qMax(EditorSettings::minimumLineHeightPixels, value.editor.fontSizePixels));
    editorLineHeightBox_->setValue(value.editor.lineHeightPixels);
    updatePreview();
}

void SettingsDialog::applyChanges() {
    // Applies what changed since the last call: appearance fields and extension pages. The theme
    // applies in selectThemeMode(), since another window may have changed it meanwhile.
    if (!ready_)
        return;
    const auto appearance = appearanceSettings();
    if (appearance != appliedSettings_) {
        appliedSettings_ = appearance;
        emit appearanceSettingsSaved(appearance);
        emit settingsSaved(appearance.editor);
    }
    for (SettingsPage* page : std::as_const(extensionPages_)) {
        if (page->hasChanges())
            page->apply();
    }
}

void SettingsDialog::updateThemeCards() {
    const bool followsSystem = selectedThemeMode_ == static_cast<int>(ThemeManager::Mode::System);
    {
        const QSignalBlocker blocker(followSystem_);
        followSystem_->setChecked(followsSystem);
    }
    followSystem_->setEnabled(theme_ != nullptr);
    // While following the system, the card of the theme in use stays marked.
    const int shownMode =
        followsSystem ? static_cast<int>(currentThemePalette().dark ? ThemeManager::Mode::Dark
                                                                    : ThemeManager::Mode::Light)
                      : selectedThemeMode_;
    for (auto* card : themeCards_) {
        card->setEnabled(theme_ != nullptr);
        card->setChecked(card->property("themeMode").toInt() == shownMode);
    }
}

void SettingsDialog::selectThemeMode(const int mode) {
    // A choice is explicit: it applies even when it matches what this dialog last chose.
    selectedThemeMode_ = mode;
    if (theme_ != nullptr)
        theme_->setMode(static_cast<ThemeManager::Mode>(mode));
    applyChanges();
    updateThemeCards();
}

void SettingsDialog::reject() {
    // Closing keeps what was applied; there is nothing left to cancel.
    QDialog::reject();
}

void SettingsDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        auto* button = qobject_cast<QAbstractButton*>(focusWidget());
        if (button && button->isEnabled())
            button->click();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

} // namespace ketplus
