#pragma once

#include <QObject>
#include <QString>

#include "ui/theme/ThemeSelection.h"

class QApplication;
class QFont;

namespace ketplus {

struct ColorScheme;
class ThemeSchemeController;

struct ThemePalette final {
    bool operator==(const ThemePalette&) const = default;
    bool dark{false};
    QString appBackground;
    QString pageBackground;
    QString sidebarBackground;
    QString panelBackground;
    QString panelSubtle;
    QString surfaceRaised;
    QString surfaceHover;
    QString textMain;
    QString textSecondary;
    QString textMuted;
    QString textDisabled;
    QString border;
    QString borderStrong;
    QString interactiveBackground;
    QString interactiveHover;
    QString interactiveActive;
    QString accent;
    QString accentHover;
    QString accentActive;
    QString accentSubtle;
    QString accentMuted;
    QString focus;
    QString tabActiveText;
    // Accent as text on panels and subtle fills; readable in both themes (DS accent-text).
    QString accentText;
    // Filled primary buttons with white text: a softer blue than the accent (DS action-primary).
    QString actionPrimary;
    QString actionPrimaryHover;
    QString actionPrimaryActive;
    // Text and icons on a filled primary button (DS action-primary-ink).
    QString actionPrimaryInk;
    QString positive;
    QString warning;
    QString danger;
    QString info;
    // Dedicated Lexilla roles. Kept separate from chrome semantics so keywords,
    // types, and numbers stay distinguishable instead of sharing one indigo.
    QString syntaxKeyword;
    QString syntaxString;
    QString syntaxNumber;
    QString syntaxType;
    QString syntaxComment;
    QString syntaxAttribute;
};

// The tooltip font size of the kit's recipe at the current theme and zoom; the stylesheet
// sets it on QToolTip so the base QWidget rule does not.
[[nodiscard]] int tooltipFontPixels();

// Resolve translucent design tokens over the panel for non-alpha consumers (SVG/QSS).
[[nodiscard]] QString themeTokenColor(const QString& name, bool dark);
[[nodiscard]] ThemePalette currentThemePalette();

class ThemeManager final : public QObject {
    Q_OBJECT

  public:
    enum class Mode {
        System,
        Light,
        Dark,
    };
    Q_ENUM(Mode)

    explicit ThemeManager(QApplication& application, QObject* parent = nullptr,
                          QString schemeDirectory = {});

    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] const ThemePalette& palette() const noexcept;
    [[nodiscard]] const ColorScheme& colorScheme() const noexcept;
    [[nodiscard]] ThemeSelection schemeSelection() const;
    [[nodiscard]] bool hasSchemePreview() const;
    [[nodiscard]] QString schemeError() const;
    [[nodiscard]] bool setSchemeSelection(const ThemeSelection& selection);
    [[nodiscard]] bool commitSchemePreview();
    void previewSchemeSelection(const ThemeSelection& selection);
    // Local, in-memory import preview. Install first, then commit its catalog id.
    void previewColorScheme(const ColorScheme& scheme);
    // The chosen interface size, before the zoom.
    [[nodiscard]] int interfaceFontSizePixels() const noexcept;
    [[nodiscard]] int interfaceZoomPercent() const noexcept;
    // The application font's size: the interface size at the zoom.
    [[nodiscard]] int interfaceTextPixels() const noexcept;

  public slots:
    void setMode(Mode mode);
    void previewMode(Mode mode);
    void cancelPreview();
    void setInterfaceScale(int fontSizePixels, int zoomPercent);

  signals:
    void themeChanged();
    void schemeFailed(const QString& message);

  private:
    void apply(bool notify = true);
    void loadInterfaceFont();
    void setApplicationFont(const QFont& font);
    void restoreTooltipFont();

    QApplication& application_;
    ThemeSchemeController* schemes_{nullptr};
    ThemePalette palette_;
    QString uiFontFamily_;
    int interfaceFontSizePixels_{14};
    int interfaceZoomPercent_{100};
};

} // namespace ketplus
