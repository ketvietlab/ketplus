// SPDX-License-Identifier: MPL-2.0
#include "ui/theme/import/JetBrainsImporter.h"
#include "ui/theme/vscode/VsCodeImporter.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

using namespace ketplus;
namespace {
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read generation input");
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (bytes.isEmpty() || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot write generated output");
}
QJsonObject reportJson(const ImportReport& report) {
    QJsonArray items, diagnostics;
    QJsonObject counts{{"mapped", 0}, {"approximated", 0}, {"skipped", 0}};
    for (const auto& item : report.items) {
        const QString disposition = item.disposition == ImportDisposition::Mapped ? "mapped"
                                    : item.disposition == ImportDisposition::Approximated
                                        ? "approximated"
                                        : "skipped";
        counts[disposition] = counts[disposition].toInt() + 1;
        items.append(QJsonObject{{"disposition", disposition},
                                 {"sourcePath", item.sourcePath},
                                 {"targetPath", item.targetPath},
                                 {"reason", item.reason}});
    }
    for (const auto& diagnostic : report.diagnostics)
        diagnostics.append(QJsonObject{
            {"severity", diagnostic.severity == DiagnosticSeverity::Error ? "error" : "warning"},
            {"path", diagnostic.path},
            {"message", diagnostic.message}});
    return {{"counts", counts}, {"items", items}, {"diagnostics", diagnostics}};
}
template <typename Result>
bool select(const Result& result, const QString& path, ColorScheme& scheme, ImportReport& report) {
    if (!result.ok()) {
        QTextStream(stderr) << QJsonDocument(reportJson(result.report)).toJson();
        return false;
    }
    for (const auto& variant : result.schemes) {
        if (path.isEmpty() || variant.sourcePath == path) {
            scheme = variant.scheme;
            report = variant.report;
            return true;
        }
    }
    return false;
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 5) {
        QTextStream(stderr)
            << "Usage: generator sources.lock.json source-cache scheme-output report-output\n";
        return 2;
    }
    const auto manifest = QJsonDocument::fromJson(read(args[1])).object();
    QMap<QString, QJsonObject> repositories;
    for (const auto& value : manifest["repositories"].toArray()) {
        const auto repository = value.toObject();
        repositories.insert(repository["key"].toString(), repository);
    }
    if (!QDir().mkpath(args[3]) || !QDir().mkpath(args[4]))
        return 1;
    for (const auto& value : manifest["schemes"].toArray()) {
        const auto spec = value.toObject();
        const auto repository = repositories.value(spec["repository"].toString());
        const QString root = QDir(args[2]).absoluteFilePath(repository["key"].toString());
        const QString input = QDir(root).filePath(spec["input"].toString());
        ColorScheme scheme;
        ImportReport report;
        const QString variant = spec["variantSource"].toString();
        const bool success =
            repository["key"] == "jetbrains"
                ? select(JetBrainsImporter::importFile(input, root), variant, scheme, report)
                : select(VsCodeImporter::importFile(input, root), variant, scheme, report);
        if (!success) {
            QTextStream(stderr) << "Import failed: " << spec["slug"].toString() << '\n';
            return 1;
        }
        const QString slug = spec["slug"].toString();
        scheme.id = QStringLiteral("builtin.") + slug;
        scheme.name = spec["name"].toString();
        const auto expectedKind = spec["kind"] == "light" ? SchemeKind::Light : SchemeKind::Dark;
        if (scheme.kind != expectedKind) {
            QTextStream(stderr) << "Unexpected imported kind: " << slug << '\n';
            return 1;
        }
        const auto summary = reportJson(report);
        scheme.metadata["sourceLabel"] = spec["sourceLabel"];
        scheme.metadata["author"] = repository["author"];
        scheme.metadata["license"] = repository["license"];
        scheme.metadata["upstream"] = repository;
        QJsonArray upstreamCredits;
        for (const auto& key : spec["additionalAttribution"].toArray())
            upstreamCredits.append(repositories.value(key.toString()));
        if (!upstreamCredits.isEmpty())
            scheme.metadata["upstreamCredits"] = upstreamCredits;
        scheme.metadata["manualAdjustments"] = spec["manualAdjustments"];
        scheme.metadata["importReport"] = QJsonObject{
            {"counts", summary["counts"]}, {"path", "docs/theme-import-reports/" + slug + ".json"}};
        // No timestamp or machine path: identical pinned sources yield identical output.
        const auto bytes = ColorSchemeWriter::write(scheme);
        if (!ColorSchemeReader::read(bytes).ok())
            return 1;
        write(QDir(args[3]).filePath(slug + ".ketscheme.json"), bytes);
        write(QDir(args[4]).filePath(slug + ".json"), QJsonDocument(summary).toJson());
        QTextStream(stdout)
            << slug << ' '
            << QJsonDocument(summary["counts"].toObject()).toJson(QJsonDocument::Compact) << '\n';
    }
    return 0;
}
