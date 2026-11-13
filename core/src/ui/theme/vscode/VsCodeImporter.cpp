#include "VsCodeImport_p.h"
#include <QDir>
#include <QFileInfo>

namespace ketplus {
using namespace vscode_detail;
bool VsCodeImportResult::ok() const { return !schemes.isEmpty() && !failed(report); }
namespace {
void collect(VsCodeImportResult& result, VsCodeImportedScheme& variant, bool valid) {
    if (valid) {
        const auto checked = ColorSchemeReader::read(ColorSchemeWriter::write(variant.scheme));
        variant.report.diagnostics.append(checked.diagnostics);
        valid = checked.ok();
    }
    for (const auto& entry : variant.report.items)
        report(result.report, entry.disposition, entry.sourcePath, entry.targetPath, entry.reason);
    result.report.diagnostics.append(variant.report.diagnostics);
    if (valid)
        result.schemes.append(std::move(variant));
}
} // namespace
VsCodeImportResult VsCodeImporter::importFile(const QString& path, const QString& rootDirectory) {
    if (path.endsWith(u".vsix", Qt::CaseInsensitive))
        return importVsix(path);
    VsCodeImportResult result;
    const QFileInfo file(path);
    if (!path.endsWith(u".json", Qt::CaseInsensitive) &&
        !path.endsWith(u".jsonc", Qt::CaseInsensitive)) {
        failure(result.report, file.fileName(),
                QStringLiteral("Expected JSON/JSONC theme or local VSIX"));
        return result;
    }
    const auto root =
        rootDirectory.isEmpty() ? file.absolutePath() : QFileInfo(rootDirectory).absoluteFilePath();
    ket::theme::io::IncludeResolver resolver(root);
    Session session{resolver};
    VsCodeImportedScheme variant;
    const bool valid =
        session.convert(QDir(root).relativeFilePath(file.absoluteFilePath()), {}, {}, variant);
    collect(result, variant, valid);
    return result;
}
VsCodeImportResult VsCodeImporter::importVsix(const QString& path) {
    VsCodeImportResult result;
    ket::theme::io::ZipArchive archive;
    ket::theme::io::Error ioError;
    if (!archive.open(path, &ioError)) {
        failure(result.report, QFileInfo(path).fileName(), ioError.message);
        return result;
    }
    ket::theme::io::IncludeResolver resolver(archive);
    Session session{resolver};
    QByteArray bytes;
    const auto manifestPath = QStringLiteral("extension/package.json");
    if (!session.read(manifestPath, bytes, result.report))
        return result;
    const auto manifest = ket::theme::io::parseJsonc(bytes, &ioError);
    if (!manifest.isObject()) {
        failure(result.report, manifestPath,
                ioError.message.isEmpty() ? QStringLiteral("Expected manifest object")
                                          : ioError.message);
        return result;
    }
    const auto object = manifest.object();
    const auto themes =
        object.value(QStringLiteral("contributes")).toObject().value(QStringLiteral("themes"));
    if (!themes.isArray() || themes.toArray().isEmpty()) {
        failure(result.report, manifestPath,
                QStringLiteral("Manifest contributes.themes must be a nonempty array"));
        return result;
    }
    const auto array = themes.toArray();
    if (array.size() > ket::theme::io::MaxEntries) {
        failure(result.report, manifestPath, QStringLiteral("Too many theme variants"));
        return result;
    }
    for (qsizetype i = 0; i < array.size(); ++i) {
        const auto source =
            manifestPath + QStringLiteral("/contributes/themes/") + QString::number(i);
        const auto entry = array[i].toObject();
        if (!entry.value(QStringLiteral("path")).isString()) {
            failure(result.report, source, QStringLiteral("Variant requires a local path string"));
            continue;
        }
        QString resource;
        if (!resolver.resolve(manifestPath, entry.value(QStringLiteral("path")).toString(),
                              &resource, &ioError)) {
            failure(result.report, source, ioError.message);
            continue;
        }
        VsCodeImportedScheme variant;
        const bool valid =
            session.convert(resource, entry.value(QStringLiteral("label")).toString(),
                            entry.value(QStringLiteral("uiTheme")).toString(), variant);
        if (valid && object.value(QStringLiteral("license")).isString()) {
            variant.scheme.metadata.insert(QStringLiteral("license"),
                                           object.value(QStringLiteral("license")));
            report(variant.report, ImportDisposition::Mapped,
                   manifestPath + QStringLiteral("/license"), QStringLiteral("metadata.license"),
                   QStringLiteral(
                       "Declared package license copied; no license verification is implied"));
        }
        collect(result, variant, valid);
        if (!session.remaining)
            break;
    }
    report(result.report, ImportDisposition::Skipped, manifestPath, {},
           QStringLiteral("Only contributes.themes data read; extension code and other "
                          "contributions are never loaded"));
    if (result.schemes.isEmpty() && !failed(result.report))
        failure(result.report, manifestPath, QStringLiteral("No supported theme variants"));
    return result;
}
} // namespace ketplus
