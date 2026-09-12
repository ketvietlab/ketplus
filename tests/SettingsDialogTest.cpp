#include "app/SettingsDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class TestSettingsPage final : public ketplus::SettingsPage {
  public:
    TestSettingsPage(QString id = QStringLiteral("test"),
                     QString title = QStringLiteral("Test page"), QWidget* parent = nullptr)
        : SettingsPage(std::move(id), std::move(title), parent) {}

    [[nodiscard]] bool hasChanges() const override { return dirty; }
    void apply() override {
        ++applyCount;
        dirty = false;
    }

    void markDirty() {
        dirty = true;
        emit changed();
    }

    bool dirty{false};
    int applyCount{0};
};

} // namespace

class SettingsDialogTest final : public QObject {
    Q_OBJECT

  private slots:
    void editsAndResetsTypography();
    void persistsAndNormalizesAppearance();
    void previewsAndCancelsThemeSelection();
    void extensionPagesDriveDirtyState();
    void validatesPageRegistrationWithoutTransferringFailedOwnership();
    void ordersAndSelectsRegisteredPagesByStableIdentity();
    void removesDestroyedPageAndFallsBackToGeneral();
};

void SettingsDialogTest::editsAndResetsTypography() {
    auto initial = ketplus::EditorSettings::defaults();
    initial.fontSizePixels = 15;
    initial.lineHeightPixels = 28;
    ketplus::SettingsDialog dialog(initial);

    auto* fontBox = dialog.findChild<QComboBox*>(QStringLiteral("editorFontFamily"));
    auto* fontSizeBox = dialog.findChild<QSpinBox*>(QStringLiteral("editorFontSize"));
    auto* lineHeightBox = dialog.findChild<QSpinBox*>(QStringLiteral("editorLineHeight"));
    auto* interfaceSizeBox = dialog.findChild<QSpinBox*>(QStringLiteral("interfaceFontSize"));
    auto* terminalSizeBox = dialog.findChild<QSpinBox*>(QStringLiteral("terminalFontSize"));
    auto* terminalLineBox = dialog.findChild<QSpinBox*>(QStringLiteral("terminalLineHeight"));
    auto* previewSizeBox = dialog.findChild<QSpinBox*>(QStringLiteral("previewFontSize"));
    auto* previewLineBox = dialog.findChild<QSpinBox*>(QStringLiteral("previewLineHeight"));
    QVERIFY(fontBox != nullptr);
    QVERIFY(fontSizeBox != nullptr);
    QVERIFY(lineHeightBox != nullptr);
    QVERIFY(interfaceSizeBox != nullptr);
    QVERIFY(terminalSizeBox != nullptr);
    QVERIFY(terminalLineBox != nullptr);
    QVERIFY(previewSizeBox != nullptr);
    QVERIFY(previewLineBox != nullptr);
    QCOMPARE(fontBox->property("fontListLoaded").toBool(), false);
    QCOMPARE(fontBox->count(), 1);
    QCOMPARE(fontSizeBox->value(), 15);
    QCOMPARE(lineHeightBox->value(), 28);

    fontSizeBox->setValue(17);
    lineHeightBox->setValue(31);
    interfaceSizeBox->setValue(16);
    terminalSizeBox->setValue(14);
    terminalLineBox->setValue(23);
    previewSizeBox->setValue(17);
    previewLineBox->setValue(27);
    QCOMPARE(dialog.settings().fontSizePixels, 17);
    QCOMPARE(dialog.settings().lineHeightPixels, 31);
    QCOMPARE(dialog.appearanceSettings().interfaceFontSizePixels, 16);
    QCOMPARE(dialog.appearanceSettings().terminal, (ketplus::TextTypographySettings{14, 23}));
    QCOMPARE(dialog.appearanceSettings().preview, (ketplus::TextTypographySettings{17, 27}));

    fontBox->showPopup();
    QCOMPARE(fontBox->property("fontListLoaded").toBool(), true);
    QVERIFY(fontBox->count() >= 1);
    fontBox->hidePopup();

    QPushButton* resetButton = nullptr;
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->property("kvRole") == QStringLiteral("settingsReset")) {
            resetButton = button;
            break;
        }
    }
    QVERIFY(resetButton != nullptr);
    resetButton->click();
    const auto resetSettings = dialog.settings();
    const auto defaults = ketplus::EditorSettings::defaults();
    QCOMPARE(resetSettings.fontSizePixels, defaults.fontSizePixels);
    QCOMPARE(resetSettings.lineHeightPixels, defaults.lineHeightPixels);
    QCOMPARE(dialog.appearanceSettings(), ketplus::AppearanceSettings::defaults());
    if (fontBox->count() > 0) {
        QCOMPARE(resetSettings.fontFamily, defaults.fontFamily);
    }
}

void SettingsDialogTest::persistsAndNormalizesAppearance() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings storage(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    auto settings = ketplus::AppearanceSettings::defaults();
    settings.interfaceFontSizePixels = 99;
    settings.editor.fontSizePixels = 18;
    settings.editor.lineHeightPixels = 17;
    settings.terminal = {16, 12};
    settings.preview = {17, 31};
    settings.save(storage);
    storage.sync();

    const auto loaded = ketplus::AppearanceSettings::load(storage);
    QCOMPARE(loaded.interfaceFontSizePixels,
             ketplus::AppearanceSettings::maximumInterfaceFontSizePixels);
    QCOMPARE(loaded.editor.fontSizePixels, 18);
    QCOMPARE(loaded.editor.lineHeightPixels, 18);
    QCOMPARE(loaded.terminal, (ketplus::TextTypographySettings{16, 16}));
    QCOMPARE(loaded.preview, (ketplus::TextTypographySettings{17, 31}));
}

void SettingsDialogTest::previewsAndCancelsThemeSelection() {
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SettingsDialogTest"));
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults(), theme);

    auto* themes = dialog.findChild<QListWidget*>(QStringLiteral("installedThemeList"));
    QVERIFY(themes != nullptr);
    QListWidgetItem* darkTheme = nullptr;
    for (int index = 0; index < themes->count(); ++index) {
        if (themes->item(index)->data(Qt::UserRole).toInt() ==
            static_cast<int>(ketplus::ThemeManager::Mode::Dark)) {
            darkTheme = themes->item(index);
            break;
        }
    }
    QVERIFY(darkTheme != nullptr);
    themes->setCurrentItem(darkTheme);
    QVERIFY(theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Light);

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(buttons != nullptr);
    buttons->button(QDialogButtonBox::Cancel)->click();
    QVERIFY(!theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Light);
}

void SettingsDialogTest::extensionPagesDriveDirtyState() {
    ketplus::SettingsDialog dialog(ketplus::EditorSettings::defaults());
    auto* page = new TestSettingsPage;
    dialog.addPage(page);

    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    QVERIFY(buttons != nullptr);
    auto* save = buttons->button(QDialogButtonBox::Save);
    if (save == nullptr) {
        for (auto* button : buttons->findChildren<QPushButton*>()) {
            if (button->property("kvRole") == QStringLiteral("primaryAction")) {
                save = button;
                break;
            }
        }
    }
    QVERIFY(save != nullptr);
    QVERIFY(!save->isEnabled());

    page->markDirty();
    QVERIFY(save->isEnabled());
    save->click();
    QCOMPARE(page->applyCount, 1);
}

void SettingsDialogTest::validatesPageRegistrationWithoutTransferringFailedOwnership() {
    ketplus::SettingsDialog dialog(ketplus::EditorSettings::defaults());
    QCOMPARE(dialog.registerPage({{}, nullptr}), ketplus::SettingsRegistrationResult::MissingPage);

    auto* invalid = new TestSettingsPage(QStringLiteral("Bad//Id"), QStringLiteral("Invalid"));
    QCOMPARE(dialog.registerPage({{invalid->id(), invalid->title(), {}, 0}, invalid}),
             ketplus::SettingsRegistrationResult::InvalidId);
    QVERIFY(invalid->parent() == nullptr);
    delete invalid;

    auto* reserved = new TestSettingsPage(QStringLiteral("general"), QStringLiteral("General"));
    QCOMPARE(dialog.registerPage({{reserved->id(), reserved->title(), {}, 0}, reserved}),
             ketplus::SettingsRegistrationResult::DuplicateId);
    QVERIFY(reserved->parent() == nullptr);
    delete reserved;

    auto* mismatch = new TestSettingsPage(QStringLiteral("private/archive"),
                                          QStringLiteral("Archived"));
    QCOMPARE(dialog.registerPage({{mismatch->id(), QStringLiteral("Other"), {}, 0}, mismatch}),
             ketplus::SettingsRegistrationResult::MetadataMismatch);
    QVERIFY(mismatch->parent() == nullptr);
    delete mismatch;

    QWidget owner;
    auto* parented = new TestSettingsPage(QStringLiteral("private/owned"),
                                          QStringLiteral("Owned"), &owner);
    QCOMPARE(dialog.registerPage({{parented->id(), parented->title(), {}, 0}, parented}),
             ketplus::SettingsRegistrationResult::AlreadyParented);

    auto* page = new TestSettingsPage(QStringLiteral("private/archive"),
                                      QStringLiteral("Archived"));
    QCOMPARE(dialog.registerPage({{page->id(), page->title(), {}, 10}, page}),
             ketplus::SettingsRegistrationResult::Registered);
    QVERIFY(page->parent() != nullptr);
    QCOMPARE(dialog.registerPage({{page->id(), page->title(), {}, 10}, page}),
             ketplus::SettingsRegistrationResult::PointerAlreadyRegistered);

    auto* duplicate = new TestSettingsPage(QStringLiteral("private/archive"),
                                           QStringLiteral("Duplicate"));
    QCOMPARE(dialog.registerPage({{duplicate->id(), duplicate->title(), {}, 0}, duplicate}),
             ketplus::SettingsRegistrationResult::DuplicateId);
    QVERIFY(duplicate->parent() == nullptr);
    delete duplicate;
}

void SettingsDialogTest::ordersAndSelectsRegisteredPagesByStableIdentity() {
    ketplus::SettingsDialog dialog(ketplus::EditorSettings::defaults());
    auto* later = new TestSettingsPage(QStringLiteral("private/later"), QStringLiteral("Later"));
    auto* first = new TestSettingsPage(QStringLiteral("private/first"), QStringLiteral("First"));
    auto* tied = new TestSettingsPage(QStringLiteral("private/tied"), QStringLiteral("Tied"));
    QCOMPARE(dialog.registerPage({{later->id(), later->title(), {}, 20}, later}),
             ketplus::SettingsRegistrationResult::Registered);
    QCOMPARE(dialog.registerPage({{first->id(), first->title(), {}, -1}, first}),
             ketplus::SettingsRegistrationResult::Registered);
    QCOMPARE(dialog.registerPage({{tied->id(), tied->title(), {}, 20}, tied}),
             ketplus::SettingsRegistrationResult::Registered);

    auto* navigation = dialog.findChild<QListWidget*>();
    QVERIFY(navigation != nullptr);
    QCOMPARE(navigation->count(), 5);
    QCOMPARE(navigation->item(2)->data(Qt::UserRole).toString(), QStringLiteral("private/first"));
    QCOMPARE(navigation->item(3)->data(Qt::UserRole).toString(), QStringLiteral("private/later"));
    QCOMPARE(navigation->item(4)->data(Qt::UserRole).toString(), QStringLiteral("private/tied"));
    QVERIFY(dialog.selectPage(QStringLiteral("private/tied")));
    QCOMPARE(dialog.selectedPageId(), QStringLiteral("private/tied"));
    QVERIFY(!dialog.selectPage(QStringLiteral("missing")));
    QCOMPARE(dialog.selectedPageId(), QStringLiteral("private/tied"));
}

void SettingsDialogTest::removesDestroyedPageAndFallsBackToGeneral() {
    ketplus::SettingsDialog dialog(ketplus::EditorSettings::defaults());
    auto* page = new TestSettingsPage(QStringLiteral("private/archive"), QStringLiteral("Archived"));
    QCOMPARE(dialog.registerPage({{page->id(), page->title(), {}, 0}, page}),
             ketplus::SettingsRegistrationResult::Registered);
    QVERIFY(dialog.selectPage(page->id()));
    delete page;
    QCOMPARE(dialog.selectedPageId(), QStringLiteral("general"));
    QVERIFY(!dialog.selectPage(QStringLiteral("private/archive")));
}

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
