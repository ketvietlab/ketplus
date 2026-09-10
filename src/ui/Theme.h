#pragma once

#include <QObject>
#include <QString>

class QApplication;

namespace ketplus {

struct ThemePalette final {
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
    QString positive;
    QString warning;
    QString danger;
    QString info;
};

class ThemeManager final : public QObject {
    Q_OBJECT

  public:
    enum class Mode {
        System,
        Light,
        Dark,
    };
    Q_ENUM(Mode)

    explicit ThemeManager(QApplication& application, QObject* parent = nullptr);

    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] const ThemePalette& palette() const noexcept;

  public slots:
    void setMode(Mode mode);

  signals:
    void themeChanged();

  private:
    void apply();
    [[nodiscard]] bool useDarkPalette() const;
    void loadInterFont();

    QApplication& application_;
    Mode mode_{Mode::System};
    ThemePalette palette_;
    QString uiFontFamily_;
};

} // namespace ketplus
