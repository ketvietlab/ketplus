#include "JetBrainsImporter.h"
#include "JetBrainsImport_p.h"
#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace ketplus {
using namespace jetbrains_detail;
bool JetBrainsImportResult::ok() const { return !schemes.isEmpty() && !hasErrors(report); }
namespace {
void finish(JetBrainsImportResult& result, JetBrainsImportedScheme& variant, bool parsed) {
    if (parsed) {
        const auto verified = ColorSchemeReader::read(ColorSchemeWriter::write(variant.scheme));
        variant.report.diagnostics.append(verified.diagnostics);
        parsed = verified.ok();
    }
    for (const auto& entry : variant.report.items)
        item(result.report, entry.disposition, entry.sourcePath, entry.targetPath, entry.reason);
    result.report.diagnostics.append(variant.report.diagnostics);
    if (parsed)
        result.schemes.append(std::move(variant));
}
} // namespace
JetBrainsImportResult JetBrainsImporter::importFile(const QString& path,
                                                    const QString& rootDirectory) {
    if (path.endsWith(u".jar", Qt::CaseInsensitive))
        return importArchive(path);
    JetBrainsImportResult result;
    const QFileInfo info(path);
    const auto root =
        rootDirectory.isEmpty() ? info.absolutePath() : QFileInfo(rootDirectory).absoluteFilePath();
    ket::theme::io::IncludeResolver resolver(root);
    Source source{resolver};
    const auto relativePath = QDir(root).relativeFilePath(info.absoluteFilePath());
    QByteArray bytes;
    if (!source.read(relativePath, bytes, result.report))
        return result;
    JetBrainsImportedScheme variant;
    const bool parsed = path.endsWith(u".theme.json", Qt::CaseInsensitive)
                            ? parseTheme(source, bytes, relativePath, variant)
                        : (path.endsWith(u".icls", Qt::CaseInsensitive) ||
                           path.endsWith(u".xml", Qt::CaseInsensitive))
                            ? parseEditor(bytes, relativePath, variant)
                            : false;
    if (!parsed && variant.report.diagnostics.isEmpty())
        error(variant.report, info.fileName(),
              QStringLiteral(
                  "Unsupported JetBrains input; expected .icls, .xml, .theme.json or .jar"));
    finish(result, variant, parsed);
    return result;
}
JetBrainsImportResult JetBrainsImporter::importArchive(const QString& path) {
    JetBrainsImportResult result;
    ket::theme::io::ZipArchive archive;
    ket::theme::io::Error ioError;
    if (!archive.open(path, &ioError)) {
        error(result.report, QFileInfo(path).fileName(), ioError.message);
        return result;
    }
    ket::theme::io::IncludeResolver resolver(archive);
    Source source{resolver};
    const auto files = archive.files();
    QSet<QString> consumed;
    for (const auto& file : files) {
        if (!file.endsWith(u".theme.json", Qt::CaseInsensitive))
            continue;
        QByteArray bytes;
        if (!source.read(file, bytes, result.report))
            break;
        JetBrainsImportedScheme variant;
        QString editorPath;
        const bool parsed = parseTheme(source, bytes, file, variant, &editorPath);
        if (!editorPath.isEmpty())
            consumed.insert(editorPath);
        finish(result, variant, parsed);
    }
    for (const auto& file : files) {
        const bool icls = file.endsWith(u".icls", Qt::CaseInsensitive);
        if (consumed.contains(file) || (!icls && !file.endsWith(u".xml", Qt::CaseInsensitive)))
            continue;
        QByteArray bytes;
        if (!source.read(file, bytes, result.report))
            break;
        // Plugin descriptors and other XML are data only, never loaded as code.
        ket::theme::io::XmlNode root;
        if (!ket::theme::io::parseXml(bytes, &root, &ioError) || root.name != u"scheme") {
            if (icls)
                error(result.report, file,
                      ioError.message.isEmpty() ? QStringLiteral("Expected <scheme> root")
                                                : ioError.message);
            else
                item(result.report, ImportDisposition::Skipped, file, {},
                     QStringLiteral("Non-scheme or unsafe XML resource ignored; plugin "
                                    "declarations are never executed"));
            continue;
        }
        JetBrainsImportedScheme variant;
        const bool parsed = parseEditor(bytes, file, variant);
        finish(result, variant, parsed);
    }
    item(result.report, ImportDisposition::Skipped, QFileInfo(path).fileName(), {},
         QStringLiteral("Plugin bytecode, extension declarations and other resources are never "
                        "executed or installed"));
    if (result.schemes.isEmpty() && !hasErrors(result.report))
        error(result.report, QFileInfo(path).fileName(),
              QStringLiteral("Archive contains no supported JetBrains color schemes"));
    return result;
}
} // namespace ketplus
