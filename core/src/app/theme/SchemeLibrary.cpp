#include "app/theme/SchemeLibrary.h"
#include "ui/theme/import/JetBrainsImporter.h"
#include "ui/theme/vscode/VsCodeImporter.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>

namespace ketplus {
bool SchemeImportBatch::ok() const {
    return !candidates.isEmpty() &&
           std::none_of(diagnostics.cbegin(), diagnostics.cend(),
                        [](const auto& d) { return d.severity == DiagnosticSeverity::Error; });
}
SchemeLibrary::SchemeLibrary(QString directory) : catalog_(std::move(directory)) {}
SchemeImportBatch SchemeLibrary::inspect(const QString& path) {
    SchemeImportBatch batch;
    const QString name = QFileInfo(path).fileName().toLower();
    if (name.endsWith(u".ketscheme.json")) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            batch.diagnostics.append({DiagnosticSeverity::Error, path, file.errorString()});
            return batch;
        }
        auto result = ColorSchemeReader::read(file.read(ColorSchemeReader::maximumBytes + 1));
        batch.diagnostics = result.diagnostics;
        if (result.ok()) {
            ImportReport report;
            report.diagnostics = result.diagnostics;
            report.items.append({ImportDisposition::Mapped, QStringLiteral("$"),
                                 QStringLiteral("$"),
                                 QStringLiteral("Native KetPlus scheme; no conversion required")});
            batch.candidates.append({result.scheme, report});
        }
    } else if (name.endsWith(u".icls") || name.endsWith(u".theme.json") || name.endsWith(u".jar")) {
        const auto result = JetBrainsImporter::importFile(path);
        batch.diagnostics = result.report.diagnostics;
        for (const auto& variant : result.schemes)
            batch.candidates.append({variant.scheme, variant.report});
    } else if (name.endsWith(u".json") || name.endsWith(u".jsonc") || name.endsWith(u".vsix")) {
        const auto result = VsCodeImporter::importFile(path);
        batch.diagnostics = result.report.diagnostics;
        for (const auto& variant : result.schemes)
            batch.candidates.append({variant.scheme, variant.report});
    } else {
        batch.diagnostics.append({DiagnosticSeverity::Error, path,
                                  QStringLiteral("Unsupported color scheme file type")});
    }
    return batch;
}
ColorSchemeCatalogResult
SchemeLibrary::importCandidate(const SchemeImportCandidate& candidate) const {
    auto scheme = candidate.scheme;
    // Imports always receive a local identity, including exported built-ins and repeat imports.
    scheme.id.clear();
    return catalog_.add(std::move(scheme));
}
bool SchemeLibrary::exportScheme(const QString& id, const QString& path,
                                 QList<SchemeDiagnostic>* diagnostics) const {
    const auto result = catalog_.load(id);
    if (!result.success) {
        if (diagnostics)
            *diagnostics = result.diagnostics;
        return false;
    }
    const auto bytes = ColorSchemeWriter::write(result.scheme, diagnostics);
    if (bytes.isEmpty())
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (diagnostics)
            diagnostics->append(
                {DiagnosticSeverity::Error, path,
                 QStringLiteral("Cannot atomically export scheme: ") + file.errorString()});
        return false;
    }
    return true;
}
QString SchemeLibrary::sourceLabel(const ColorScheme& scheme) {
    const auto app = scheme.metadata.value(QStringLiteral("sourceApp")).toString();
    if (app == u"VS Code" || app == u"JetBrains")
        return app;
    return QStringLiteral("KetPlus");
}
QString SchemeLibrary::diagnosticText(const QList<SchemeDiagnostic>& diagnostics) {
    QStringList lines;
    for (const auto& d : diagnostics)
        lines.append(d.path.isEmpty() ? d.message : d.path + QStringLiteral(": ") + d.message);
    return lines.join(u'\n');
}
} // namespace ketplus
