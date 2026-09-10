#include "app/SettingsDialog.h"

#include <QFontComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QtTest>

class SettingsDialogTest final : public QObject {
    Q_OBJECT

  private slots:
    void editsAndResetsTypography();
};

void SettingsDialogTest::editsAndResetsTypography() {
    auto initial = ketplus::EditorSettings::defaults();
    initial.fontSizePixels = 15;
    initial.lineHeightPixels = 28;
    ketplus::SettingsDialog dialog(initial);

    auto* fontBox = dialog.findChild<QFontComboBox*>(QStringLiteral("editorFontFamily"));
    auto* fontSizeBox = dialog.findChild<QSpinBox*>(QStringLiteral("editorFontSize"));
    auto* lineHeightBox = dialog.findChild<QSpinBox*>(QStringLiteral("editorLineHeight"));
    QVERIFY(fontBox != nullptr);
    QVERIFY(fontSizeBox != nullptr);
    QVERIFY(lineHeightBox != nullptr);
    QCOMPARE(fontSizeBox->value(), 15);
    QCOMPARE(lineHeightBox->value(), 28);

    fontSizeBox->setValue(17);
    lineHeightBox->setValue(31);
    QCOMPARE(dialog.settings().fontSizePixels, 17);
    QCOMPARE(dialog.settings().lineHeightPixels, 31);

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
    if (fontBox->count() > 0) {
        QCOMPARE(resetSettings.fontFamily, defaults.fontFamily);
    }
}

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
