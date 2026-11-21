#pragma once
#include "JetBrainsImporter.h"
#include "ui/theme/io/ThemeInput.h"

namespace ketplus::jetbrains_detail {
void item(ImportReport& report, ImportDisposition disposition, const QString& source,
          const QString& target, const QString& reason);
void error(ImportReport& report, const QString& source, const QString& message);
bool hasErrors(const ImportReport& report);
std::optional<QColor> color(QString value, bool xml);
void setColor(ColorScheme& scheme, const QString& target, const QColor& value);
ColorScheme newScheme(const QString& path, const QByteArray& bytes, const QString& name);
void setKind(ColorScheme& scheme, bool dark);
bool parseEditor(const QByteArray& bytes, const QString& path, JetBrainsImportedScheme& output);
struct Source {
    const ket::theme::io::IncludeResolver& resolver;
    quint64 bytesRead = 0;
    bool read(const QString& path, QByteArray& data, ImportReport& report);
};
bool parseTheme(Source& source, const QByteArray& bytes, const QString& path,
                JetBrainsImportedScheme& output, QString* editorPath = nullptr);
} // namespace ketplus::jetbrains_detail
