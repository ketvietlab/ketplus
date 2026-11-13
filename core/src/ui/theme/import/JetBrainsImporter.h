#pragma once
#include "ui/theme/ColorScheme.h"

namespace ketplus {
struct JetBrainsImportedScheme {
    ColorScheme scheme;
    ImportReport report;
    QString sourcePath;
};
struct JetBrainsImportResult {
    QList<JetBrainsImportedScheme> schemes;
    ImportReport report;
    [[nodiscard]] bool ok() const;
};
// Conversion only: no installation, plugin execution or network access.
class JetBrainsImporter {
  public:
    // rootDirectory is an explicit caller-selected dependency boundary; defaults
    // to the selected file's directory. Absolute editorScheme resource names
    // refer to this boundary, never to the host filesystem root.
    [[nodiscard]] static JetBrainsImportResult importFile(const QString& path,
                                                          const QString& rootDirectory = {});
    [[nodiscard]] static JetBrainsImportResult importArchive(const QString& path);
};
} // namespace ketplus
