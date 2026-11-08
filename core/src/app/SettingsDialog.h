#pragma once

#include "app/AppearanceSettings.h"

#include <QDialog>
#include <QString>
#include <QWidget>

class QAbstractButton;
class QDialogButtonBox;
class QComboBox;
class QListWidget;
namespace ket {
class StepSlider;
class ThemeCard;
} // namespace ket
class QStackedWidget;

namespace ketplus {

class ThemeManager;
class PixelSelect;
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

struct SettingsPageDescriptor final {
    QString id;
    QString title;
    QString sectionId;
    int order{0};
};

enum class SettingsRegistrationResult {
    Registered,
    InvalidId,
    DuplicateId,
    MissingPage,
    AlreadyParented,
    PointerAlreadyRegistered,
    MetadataMismatch,
};

struct SettingsPageRegistration final {
    SettingsPageDescriptor descriptor;
    SettingsPage* page{nullptr};
};

class SettingsHost {
  public:
    virtual ~SettingsHost() = default;
    virtual SettingsRegistrationResult registerPage(SettingsPageRegistration registration) = 0;
    virtual bool selectPage(const QString& id) = 0;
    [[nodiscard]] virtual QString selectedPageId() const = 0;
};

class SettingsDialog final : public QDialog, public SettingsHost {
    Q_OBJECT

  public:
    explicit SettingsDialog(const AppearanceSettings& settings, ThemeManager& theme,
                            QWidget* parent = nullptr);
    explicit SettingsDialog(const EditorSettings& settings, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    [[nodiscard]] AppearanceSettings appearanceSettings() const;
    [[nodiscard]] EditorSettings settings() const;
    void addPage(SettingsPage* page);
    SettingsRegistrationResult registerPage(SettingsPageRegistration registration) override;
    bool selectPage(const QString& id) override;
    [[nodiscard]] QString selectedPageId() const override;

  signals:
    void appearanceSettingsSaved(const ketplus::AppearanceSettings& settings);
    void settingsSaved(const ketplus::EditorSettings& settings);

  protected:
    void reject() override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    void initialize(const AppearanceSettings& settings);
    QWidget* createAppearancePage();
    QWidget* createThemeCard();
    QWidget* createTypographyCard();
    void addNavigationPage(const QString& id, const QString& title, QWidget* page);
    void setAppearanceSettings(const AppearanceSettings& settings);
    void updatePreview();
    void updateDirtyState();
    void updateThemePreview();
    void selectThemeMode(int mode);
    void rebuildExtensionNavigation();

    struct RegisteredPage final {
        SettingsPageDescriptor descriptor;
        SettingsPage* page{nullptr};
        qsizetype sequence{0};
    };

    ThemeManager* theme_{nullptr};
    AppearanceSettings initialSettings_;
    int initialThemeMode_{0};
    int selectedThemeMode_{0};
    QListWidget* navigation_{nullptr};
    QStackedWidget* pages_{nullptr};
    QAbstractButton* followSystem_{nullptr};
    QList<ket::ThemeCard*> themeCards_;
    QComboBox* editorFontBox_{nullptr};
    QString editorFontFamily_;
    // Kept as loaded: the dialog has no terminal font picker yet.
    QString terminalFontFamily_;
    PixelSelect* interfaceFontSizeBox_{nullptr};
    PixelSelect* editorFontSizeBox_{nullptr};
    PixelSelect* editorLineHeightBox_{nullptr};
    PixelSelect* terminalFontSizeBox_{nullptr};
    PixelSelect* terminalLineHeightBox_{nullptr};
    PixelSelect* previewFontSizeBox_{nullptr};
    PixelSelect* previewLineHeightBox_{nullptr};
    TypographyPreview* preview_{nullptr};
    QWidget* buttons_{nullptr};
    ket::StepSlider* zoomStep_{nullptr};
    QAbstractButton* resetButton_{nullptr};
    QAbstractButton* saveButton_{nullptr};
    QList<SettingsPage*> extensionPages_;
    QList<RegisteredPage> registeredPages_;
    qsizetype nextRegistrationSequence_{0};
};

} // namespace ketplus
