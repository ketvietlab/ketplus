#pragma once

#include "app/AppearanceSettings.h"

#include <QDialog>
#include <QWidget>

class QCheckBox;
class QDialogButtonBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;

namespace ketplus {

class ThemeManager;
class TypographyPreview;

class SettingsPage : public QWidget {
    Q_OBJECT

  public:
    explicit SettingsPage(QString id, QString title, QWidget* parent = nullptr);

    [[nodiscard]] const QString& id() const noexcept;
    [[nodiscard]] const QString& title() const noexcept;
    [[nodiscard]] virtual bool hasChanges() const;
    virtual void resetToDefaults();
    virtual void apply();

  signals:
    void changed();

  private:
    QString id_;
    QString title_;
};

class SettingsDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit SettingsDialog(const AppearanceSettings& settings, ThemeManager& theme,
                            QWidget* parent = nullptr);
    explicit SettingsDialog(const EditorSettings& settings, QWidget* parent = nullptr);

    [[nodiscard]] AppearanceSettings appearanceSettings() const;
    [[nodiscard]] EditorSettings settings() const;
    void addPage(SettingsPage* page);

  signals:
    void appearanceSettingsSaved(const ketplus::AppearanceSettings& settings);
    void settingsSaved(const ketplus::EditorSettings& settings);

  protected:
    void reject() override;

  private:
    void initialize(const AppearanceSettings& settings);
    QWidget* createGeneralPage();
    QWidget* createAppearancePage();
    QWidget* createThemeCard();
    QWidget* createTypographyCard();
    void addNavigationPage(const QString& id, const QString& title, QWidget* page);
    void setAppearanceSettings(const AppearanceSettings& settings);
    void updatePreview();
    void updateDirtyState();
    void updateThemePreview();
    void selectThemeMode(int mode);

    ThemeManager* theme_{nullptr};
    AppearanceSettings initialSettings_;
    int initialThemeMode_{0};
    int selectedThemeMode_{0};
    QListWidget* navigation_{nullptr};
    QStackedWidget* pages_{nullptr};
    QLineEdit* themeSearch_{nullptr};
    QCheckBox* followSystem_{nullptr};
    QListWidget* themeList_{nullptr};
    QComboBox* editorFontBox_{nullptr};
    QString editorFontFamily_;
    QSpinBox* interfaceFontSizeBox_{nullptr};
    QSpinBox* editorFontSizeBox_{nullptr};
    QSpinBox* editorLineHeightBox_{nullptr};
    QSpinBox* terminalFontSizeBox_{nullptr};
    QSpinBox* terminalLineHeightBox_{nullptr};
    QSpinBox* previewFontSizeBox_{nullptr};
    QSpinBox* previewLineHeightBox_{nullptr};
    TypographyPreview* preview_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* resetButton_{nullptr};
    QPushButton* saveButton_{nullptr};
    QList<SettingsPage*> extensionPages_;
};

} // namespace ketplus
