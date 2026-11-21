#pragma once
#include "ui/theme/ColorSchemeCatalog.h"
#include "ui/theme/ThemeSelection.h"
#include <QObject>
#include <QPointer>
class QWidget;
namespace ket {
class CommandPalette;
}
namespace ketplus {
class ThemeManager;
class SchemeQuickPicker final : public QObject {
    Q_OBJECT
  public:
    SchemeQuickPicker(ThemeManager& theme, QWidget* anchor, QString directory = {});
    ~SchemeQuickPicker() override;
    void open();
    [[nodiscard]] ket::CommandPalette* palette() const;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void filter(const QString& query);
    void previewHighlighted();
    void activate(const QString& id);
    QPointer<ThemeManager> theme_;
    QPointer<ket::CommandPalette> palette_;
    QList<ColorScheme> schemes_;
    QStringList filteredIds_;
    ThemeSelection initial_;
    bool committed_{false};
};
// Shared menu/command entry point for Lite and Pro.
void showColorSchemePicker(ThemeManager& theme, QWidget* anchor);
} // namespace ketplus
