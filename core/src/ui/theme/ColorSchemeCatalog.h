#pragma once
#include "ui/theme/ColorScheme.h"

namespace ketplus {
struct ColorSchemeCatalogResult {
    ColorScheme scheme;
    QList<SchemeDiagnostic> diagnostics;
    bool success{false};
};
class ColorSchemeCatalog {
  public:
    // Empty root selects QStandardPaths::AppDataLocation/color-schemes/.
    explicit ColorSchemeCatalog(QString root = {});
    [[nodiscard]] QString directory() const;
    [[nodiscard]] QList<ColorScheme> list() const;
    // Unknown/corrupt IDs return the chosen built-in and a warning.
    [[nodiscard]] ColorSchemeCatalogResult load(const QString& id,
                                                SchemeKind fallback = SchemeKind::Light) const;
    // Adds only; never overwrites an existing or built-in ID. Empty IDs get a UUID.
    [[nodiscard]] ColorSchemeCatalogResult add(ColorScheme scheme) const;
    [[nodiscard]] ColorSchemeCatalogResult duplicate(const QString& id, const QString& name) const;
    [[nodiscard]] bool remove(const QString& id,
                              QList<SchemeDiagnostic>* diagnostics = nullptr) const;

  private:
    QString root_;
};
} // namespace ketplus
