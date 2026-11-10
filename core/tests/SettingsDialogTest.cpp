#include "app/SettingsDialog.h"
#include "KetAppearance.h"
#include "KetEmptyState.h"
#include "KetRecipes.h"
#include "KetSelect.h"
#include "ui/KetFonts.h"
#include "ui/Theme.h"
#include <QLabel>
#include <QRawFont>

#include "app/PixelSelect.h"
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QSettings>
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
    void pixelSelectPreservesRangeAndKeyboardSelection();
    void defaultFontResolvesWithoutPlatformFallback();
    void kitLabelsKeepRecipeFontsAcrossThemeChanges();
    void persistsAndNormalizesAppearance();
    void appliesThemeSelectionAtOnce();
    void reselectsAThemeAnotherWindowChanged();
    void extensionPagesApplyAtOnce();
    void appearanceChangesApplyAtOnce();
    void validatesPageRegistrationWithoutTransferringFailedOwnership();
    void ordersAndSelectsRegisteredPagesByStableIdentity();
    void removesDestroyedPageAndFallsBackToGeneral();
    void zoomDuringExtensionRebuildKeepsNavigationAlive();
};

void SettingsDialogTest::kitLabelsKeepRecipeFontsAcrossThemeChanges() {
    ketplus::ThemeManager theme(*qApp);
    ket::EmptyState empty({}, "Title", "Message");
    const auto title = [&empty] {
        for (auto* label : empty.findChildren<QLabel*>())
            if (label->text() == "Title")
                return label;
        return static_cast<QLabel*>(nullptr);
    }();
    QVERIFY(title);
    empty.show();
    for (const auto mode :
         {ketplus::ThemeManager::Mode::Light, ketplus::ThemeManager::Mode::Dark}) {
        theme.setMode(mode);
        theme.previewMode(mode);
        theme.cancelPreview();
        for (int pixels : {14, 21, 14}) {
            theme.setInterfaceFontSizePixels(pixels);
            QCoreApplication::processEvents();
            const auto recipe = ket::emptyStateRecipe(ket::Appearance::instance().theme());
            QCOMPARE(title->font().pixelSize(), qRound(recipe.titlePixels * pixels / 14.0));
            QCOMPARE(int(title->font().weight()), recipe.titleWeight);
        }
    }
}

void SettingsDialogTest::pixelSelectPreservesRangeAndKeyboardSelection() {
    ketplus::ThemeManager theme(*qApp);
    ketplus::PixelSelect field;
    field.setRange(8, 32);
    field.setValue(17);
    field.show();
    auto* select = field.findChild<ket::Select*>();
    QVERIFY(select);
    QCOMPARE(select->currentText(), QStringLiteral("17 px"));
    QSignalSpy changes(&field, &ketplus::PixelSelect::valueChanged);
    select->setFocus();
    QTest::keyClick(select, Qt::Key_Down);
    QCOMPARE(field.value(), 18);
    QCOMPARE(changes.count(), 1);
    field.setMinimum(20);
    QCOMPARE(field.value(), 20);
    QCOMPARE(changes.count(), 2);
    field.setMinimum(8);
    QCOMPARE(field.value(), 20);
    QCOMPARE(changes.count(), 2);
    field.setValue(19);
    QCOMPARE(select->currentText(), QStringLiteral("19 px"));
    field.setValue(100);
    QCOMPARE(field.value(), 32);
}

void SettingsDialogTest::defaultFontResolvesWithoutPlatformFallback() {
    ketplus::registerKetFonts();
    ketplus::ThemeManager theme(*qApp);
    auto settings = ketplus::EditorSettings::defaults();
    QCOMPARE(QRawFont::fromFont(settings.font()).familyName(), QStringLiteral("KET Sans Mono"));
    QCOMPARE(QRawFont::fromFont(settings.font()).styleName(), QStringLiteral("Medium"));
    settings.fontFamily = QStringLiteral("KÉT Sans Mono");
    QCOMPARE(settings.normalized().fontFamily, QStringLiteral("KET Sans Mono"));
    QCOMPARE(QRawFont::fromFont(settings.font()).familyName(), QStringLiteral("KET Sans Mono"));
    settings.fontFamily = QStringLiteral("Custom mono family");
    QCOMPARE(settings.normalized().fontFamily, settings.fontFamily);
}

void SettingsDialogTest::editsAndResetsTypography() {
    auto initial = ketplus::EditorSettings::defaults();
    initial.fontSizePixels = 15;
    initial.lineHeightPixels = 28;
    ketplus::SettingsDialog dialog(initial);

    auto* fontBox = dialog.findChild<QComboBox*>(QStringLiteral("editorFontFamily"));
    auto* fontSizeBox = dialog.findChild<ketplus::PixelSelect*>(QStringLiteral("editorFontSize"));
    auto* lineHeightBox =
        dialog.findChild<ketplus::PixelSelect*>(QStringLiteral("editorLineHeight"));
    auto* interfaceSizeBox =
        dialog.findChild<ketplus::PixelSelect*>(QStringLiteral("interfaceFontSize"));
    QVERIFY(fontBox != nullptr);
    QVERIFY(fontSizeBox != nullptr);
    QVERIFY(lineHeightBox != nullptr);
    QVERIFY(interfaceSizeBox != nullptr);
    // Terminal and Markdown sizes have no controls; they follow the editor and the interface.
    for (const auto* name :
         {"terminalFontSize", "terminalLineHeight", "previewFontSize", "previewLineHeight"})
        QVERIFY(dialog.findChild<ketplus::PixelSelect*>(QString::fromLatin1(name)) == nullptr);
    QCOMPARE(fontBox->property("fontListLoaded").toBool(), false);
    QCOMPARE(fontBox->count(), 1);
    QCOMPARE(fontSizeBox->value(), 15);
    QCOMPARE(lineHeightBox->value(), 28);

    fontSizeBox->setValue(17);
    lineHeightBox->setValue(31);
    interfaceSizeBox->setValue(16);
    QCOMPARE(dialog.settings().fontSizePixels, 17);
    QCOMPARE(dialog.settings().lineHeightPixels, 31);
    QCOMPARE(dialog.appearanceSettings().interfaceFontSizePixels, 16);
    QCOMPARE(dialog.appearanceSettings().terminal, (ketplus::TextTypographySettings{17, 31}));
    // One pixel over the interface, at the 15/26 rhythm: 17 px on 29 px.
    QCOMPARE(dialog.appearanceSettings().preview, (ketplus::TextTypographySettings{17, 29}));

    fontBox->showPopup();
    QCOMPARE(fontBox->property("fontListLoaded").toBool(), true);
    QVERIFY(fontBox->count() >= 1);
    fontBox->hidePopup();

    QAbstractButton* resetButton = nullptr;
    for (auto* button : dialog.findChildren<QAbstractButton*>()) {
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
    QCOMPARE(settings.editor.fontFamily, QStringLiteral("KET Sans Mono"));
    QCOMPARE(settings.editor.fontSizePixels, 14);
    settings.interfaceFontSizePixels = 99;
    settings.editor.fontSizePixels = 18;
    settings.editor.lineHeightPixels = 17;
    settings.terminal = {16, 12};
    settings.preview = {17, 31};
    settings.terminalFontFamily = QStringLiteral("  KET Sans Mono  ");
    settings.save(storage);
    storage.sync();

    const auto loaded = ketplus::AppearanceSettings::load(storage);
    QCOMPARE(loaded.interfaceFontSizePixels,
             ketplus::AppearanceSettings::maximumInterfaceFontSizePixels);
    QCOMPARE(loaded.editor.fontSizePixels, 18);
    QCOMPARE(loaded.editor.lineHeightPixels, 18);
    // Stored terminal and preview sizes are not read: they follow the editor and interface.
    QCOMPARE(loaded.terminal, (ketplus::TextTypographySettings{18, 18}));
    QCOMPARE(loaded.preview,
             (ketplus::TextTypographySettings{
                 ketplus::AppearanceSettings::maximumInterfaceFontSizePixels + 1,
                 qRound((ketplus::AppearanceSettings::maximumInterfaceFontSizePixels + 1) * 26.0 /
                        15.0)}));
    QCOMPARE(ketplus::AppearanceSettings::defaults().preview,
             (ketplus::TextTypographySettings{15, 26}));
    QCOMPARE(ketplus::AppearanceSettings::defaults().terminal,
             (ketplus::TextTypographySettings{14, 24}));
    QCOMPARE(loaded.terminalFontFamily, QStringLiteral("KET Sans Mono"));

    // The dialog has no terminal font picker, so saving it keeps the stored family.
    ketplus::ThemeManager theme(*qApp);
    ketplus::SettingsDialog dialog(loaded, theme);
    QCOMPARE(dialog.appearanceSettings().terminalFontFamily, QStringLiteral("KET Sans Mono"));
}

void SettingsDialogTest::appliesThemeSelectionAtOnce() {
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SettingsDialogTest"));
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings::defaults(), theme);
    int saved = 0;
    connect(&dialog, &ketplus::SettingsDialog::appearanceSettingsSaved, &dialog,
            [&saved](const ketplus::AppearanceSettings&) { ++saved; });

    auto* darkTheme = dialog.findChild<QAbstractButton*>(QStringLiteral("darkThemeCard"));
    QVERIFY(darkTheme != nullptr);
    darkTheme->click();
    QVERIFY(theme.palette().dark);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
    // There is nothing to save or cancel: closing keeps the chosen theme.
    QVERIFY(dialog.findChild<QAbstractButton*>(QStringLiteral("settingsSaveButton")) == nullptr);
    QVERIFY(dialog.findChild<QAbstractButton*>(QStringLiteral("settingsCancelButton")) == nullptr);
    dialog.show();
    QTest::keyClick(&dialog, Qt::Key_Escape);
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
    // The theme is not an appearance field, and opening the dialog saved nothing.
    QCOMPARE(saved, 0);
}

// Two windows open on Light: after the second picks Dark, the first follows and can pick
// Light again.
void SettingsDialogTest::reselectsAThemeAnotherWindowChanged() {
    ketplus::ThemeManager theme(*qApp);
    theme.setMode(ketplus::ThemeManager::Mode::Light);
    ketplus::SettingsDialog first(ketplus::AppearanceSettings::defaults(), theme);
    ketplus::SettingsDialog second(ketplus::AppearanceSettings::defaults(), theme);
    auto* firstLight = first.findChild<QAbstractButton*>(QStringLiteral("lightThemeCard"));
    auto* firstDark = first.findChild<QAbstractButton*>(QStringLiteral("darkThemeCard"));
    auto* secondDark = second.findChild<QAbstractButton*>(QStringLiteral("darkThemeCard"));
    QVERIFY(firstLight && firstDark && secondDark);
    QVERIFY(firstLight->isChecked());

    secondDark->click();
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Dark);
    QVERIFY(firstDark->isChecked());
    QVERIFY(!firstLight->isChecked());

    firstLight->click();
    QCOMPARE(theme.mode(), ketplus::ThemeManager::Mode::Light);
    QVERIFY(!theme.palette().dark);
}

void SettingsDialogTest::extensionPagesApplyAtOnce() {
    ketplus::SettingsDialog dialog(ketplus::EditorSettings::defaults());
    auto* page = new TestSettingsPage;
    dialog.addPage(page);
    QCOMPARE(page->applyCount, 0);
    page->markDirty();
    QCOMPARE(page->applyCount, 1);
}

void SettingsDialogTest::appearanceChangesApplyAtOnce() {
    ketplus::SettingsDialog dialog(ketplus::EditorSettings::defaults());
    QList<ketplus::AppearanceSettings> saved;
    connect(&dialog, &ketplus::SettingsDialog::appearanceSettingsSaved, &dialog,
            [&saved](const ketplus::AppearanceSettings& settings) { saved.append(settings); });
    auto* size = dialog.findChild<ketplus::PixelSelect*>(QStringLiteral("editorFontSize"));
    QVERIFY(size != nullptr);
    size->setValue(size->value() + 2);
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.constLast().editor.fontSizePixels, size->value());
    // The same value again applies nothing.
    size->setValue(size->value());
    QCOMPARE(saved.size(), 1);
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

    auto* mismatch =
        new TestSettingsPage(QStringLiteral("private/archive"), QStringLiteral("Archived"));
    QCOMPARE(dialog.registerPage({{mismatch->id(), QStringLiteral("Other"), {}, 0}, mismatch}),
             ketplus::SettingsRegistrationResult::MetadataMismatch);
    QVERIFY(mismatch->parent() == nullptr);
    delete mismatch;

    QWidget owner;
    auto* parented =
        new TestSettingsPage(QStringLiteral("private/owned"), QStringLiteral("Owned"), &owner);
    QCOMPARE(dialog.registerPage({{parented->id(), parented->title(), {}, 0}, parented}),
             ketplus::SettingsRegistrationResult::AlreadyParented);

    auto* page =
        new TestSettingsPage(QStringLiteral("private/archive"), QStringLiteral("Archived"));
    QCOMPARE(dialog.registerPage({{page->id(), page->title(), {}, 10}, page}),
             ketplus::SettingsRegistrationResult::Registered);
    QVERIFY(page->parent() != nullptr);
    QCOMPARE(dialog.registerPage({{page->id(), page->title(), {}, 10}, page}),
             ketplus::SettingsRegistrationResult::PointerAlreadyRegistered);

    auto* duplicate =
        new TestSettingsPage(QStringLiteral("private/archive"), QStringLiteral("Duplicate"));
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
    auto* page =
        new TestSettingsPage(QStringLiteral("private/archive"), QStringLiteral("Archived"));
    QCOMPARE(dialog.registerPage({{page->id(), page->title(), {}, 0}, page}),
             ketplus::SettingsRegistrationResult::Registered);
    QVERIFY(dialog.selectPage(page->id()));
    delete page;
    QCOMPARE(dialog.selectedPageId(), QStringLiteral("appearance"));
    QVERIFY(!dialog.selectPage(QStringLiteral("private/archive")));
}

void SettingsDialogTest::zoomDuringExtensionRebuildKeepsNavigationAlive() {
    ketplus::ThemeManager theme(*qApp);
    ketplus::SettingsDialog dialog(ketplus::AppearanceSettings{}, theme);
    for (int i = 0; i < 12; ++i) {
        auto* page = new TestSettingsPage(QStringLiteral("page-%1").arg(i));
        dialog.addPage(page);
        // Deliberately do not drain DeferredDelete before changing the global zoom.
        theme.setInterfaceFontSizePixels(i % 2 == 0 ? 18 : 14);
        QVERIFY(dialog.selectPage(page->id()));
        QCOMPARE(dialog.selectedPageId(), page->id());
    }
    theme.setInterfaceFontSizePixels(14);
    auto* navigation = dialog.findChild<QListWidget*>("settingsNavigation");
    QCOMPARE(navigation->count(), 14);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    theme.setInterfaceFontSizePixels(16);
    QVERIFY(dialog.selectPage("page-0"));
    QCOMPARE(dialog.selectedPageId(), QStringLiteral("page-0"));
}

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
