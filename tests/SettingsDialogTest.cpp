#include "app/SettingsDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

namespace {

class TestSettingsPage final : public ketplus::SettingsPage {
  public:
    TestSettingsPage() : SettingsPage(QStringLiteral("test"), QStringLiteral("Test page")) {}

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
};

void SettingsDialogTest::editsAndResetsTypography() {
    auto initial = ketplus::EditorSettings::defaults();
    initial.fontSizePixels = 15;
    initial.lineHeightPixels = 28;
    ketplus::SettingsDialog dialog(initial);

    auto* fontBox = dialog.findChild<QFontComboBox*>(QStringLiteral("editorFontFamily"));
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

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
