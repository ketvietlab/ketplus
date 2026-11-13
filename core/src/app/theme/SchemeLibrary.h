#pragma once
#include "ui/theme/ColorSchemeCatalog.h"

namespace ketplus {
struct SchemeImportCandidate {
    ColorScheme scheme;
    ImportReport report;
};
struct SchemeImportBatch {
    QList<SchemeImportCandidate> candidates;
    QList<SchemeDiagnostic> diagnostics;
    [[nodiscard]] bool ok() const;
};
// Shared by Settings and the keyboard picker. Parsing never changes the catalog.
class SchemeLibrary {
  public:
    explicit SchemeLibrary(QString directory = {});
    [[nodiscard]] const ColorSchemeCatalog& catalog() const { return catalog_; }
    [[nodiscard]] static SchemeImportBatch inspect(const QString& path);
    [[nodiscard]] ColorSchemeCatalogResult
    importCandidate(const SchemeImportCandidate& candidate) const;
    [[nodiscard]] bool exportScheme(const QString& id, const QString& path,
                                    QList<SchemeDiagnostic>* diagnostics) const;
    [[nodiscard]] static QString sourceLabel(const ColorScheme& scheme);
    [[nodiscard]] static QString diagnosticText(const QList<SchemeDiagnostic>& diagnostics);

  private:
    ColorSchemeCatalog catalog_;
};
} // namespace ketplus
