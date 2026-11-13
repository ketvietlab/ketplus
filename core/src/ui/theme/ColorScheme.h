#pragma once

#include "ui/Theme.h"
#include <QByteArray>
#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <optional>

namespace ketplus {

enum class SchemeKind { Light, Dark, HighContrast };
enum class DiagnosticSeverity { Warning, Error };
struct SchemeDiagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
    QString path;
    QString message;
    bool operator==(const SchemeDiagnostic&) const = default;
};
struct SyntaxStyle {
    std::optional<QColor> fg;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> underline;
    bool operator==(const SyntaxStyle&) const = default;
};
using SchemeColors = QMap<QString, QColor>;

// Sparse authored values. UI keys use group.token (e.g. sidebar.bg).
// Preserved JSON carries unrecognized data at every depth, never executable code.
struct ColorScheme {
    int schemaVersion{1};
    QString id;
    QString name;
    SchemeKind kind{SchemeKind::Light};
    QString extends{QStringLiteral("builtin-light")};
    SchemeColors ui;
    SchemeColors editor;
    QMap<QString, SyntaxStyle> syntax;
    SchemeColors terminal;
    SchemeColors diff;
    QJsonObject metadata;
    QJsonObject preserved;
    bool operator==(const ColorScheme&) const = default;
};
struct ColorSchemeReadResult {
    ColorScheme scheme;
    QList<SchemeDiagnostic> diagnostics;
    [[nodiscard]] bool ok() const;
};
class ColorSchemeReader {
  public:
    static constexpr qsizetype maximumBytes = 5 * 1024 * 1024;
    [[nodiscard]] static ColorSchemeReadResult read(const QByteArray& json);
    [[nodiscard]] static std::optional<QColor> parseColor(const QString& hex);
};
class ColorSchemeWriter {
  public:
    [[nodiscard]] static QByteArray write(const ColorScheme& scheme);
};
// A resolved value has every documented token and style property filled.
[[nodiscard]] ColorScheme builtinColorScheme(SchemeKind kind);
[[nodiscard]] ColorScheme resolveColorScheme(const ColorScheme& scheme);
[[nodiscard]] ThemePalette colorSchemePalette(const ColorScheme& scheme);

// Shared importer contract: every approximation or omission must explain why.
enum class ImportDisposition { Mapped, Approximated, Skipped };
struct ImportReportItem {
    ImportDisposition disposition{ImportDisposition::Mapped};
    QString sourcePath;
    QString targetPath;
    QString reason;
    bool operator==(const ImportReportItem&) const = default;
};
struct ImportReport {
    QList<ImportReportItem> items;
    QList<SchemeDiagnostic> diagnostics;
    bool operator==(const ImportReport&) const = default;
};
} // namespace ketplus
