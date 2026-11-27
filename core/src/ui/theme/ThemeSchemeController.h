#pragma once
#include "ui/theme/ColorScheme.h"
#include "ui/theme/ThemeSelection.h"
#include <QHash>
#include <QObject>
#include <optional>

namespace ketplus {
// Owns selection policy and prepared snapshots; consumers remain owned by their
// views. Worker jobs capture values only and cannot touch QApplication or QML.
class ThemeSchemeController final : public QObject {
    Q_OBJECT
  public:
    explicit ThemeSchemeController(QString directory, QString legacyTheme, QObject* parent);
    void start();
    [[nodiscard]] const ColorScheme& scheme() const;
    [[nodiscard]] const ThemePalette& palette() const;
    [[nodiscard]] const ThemeSelection& selection() const;
    [[nodiscard]] ThemeManager::Mode mode() const;
    [[nodiscard]] bool hasPreview() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool commit(const ThemeSelection& selection);
    [[nodiscard]] bool commitPreview();
    void preview(const ThemeSelection& selection);
    void preview(const ColorScheme& scheme);
    void cancelPreview();

  signals:
    void applied();
    void failed(const QString& message);

  private:
    struct Prepared {
        ColorScheme scheme;
        ThemePalette palette;
        QHash<QString, QColor> overrides;
    };
    static Prepared prepare(ColorScheme scheme);
    [[nodiscard]] bool useKitSystem() const;
    void request();
    void publish(Prepared prepared, quint64 generation);
    ThemeSelectionStore* store_;
    QString directory_;
    Prepared prepared_;
    bool committedDark_{false};
    bool publishing_{false};
    quint64 generation_{0};
    std::optional<ThemeSelection> previewSelection_;
    std::optional<ColorScheme> previewScheme_;
};
} // namespace ketplus
