#pragma once
#include "ui/theme/ColorScheme.h"

namespace ketplus {
struct VsCodeImportedScheme {
    ColorScheme scheme;
    ImportReport report;
    QString sourcePath;
};
struct VsCodeImportResult {
    QList<VsCodeImportedScheme> schemes;
    ImportReport report;
    [[nodiscard]] bool ok() const;
};
class VsCodeImporter {
  public:
    // Optional caller-selected root bounds includes and tmTheme resources.
    [[nodiscard]] static VsCodeImportResult importFile(const QString& path,
                                                       const QString& rootDirectory = {});
    [[nodiscard]] static VsCodeImportResult importVsix(const QString& path);
};
} // namespace ketplus
