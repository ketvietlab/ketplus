#include "ui/theme/ColorSchemeCatalog.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace ketplus {
namespace {
bool validUserId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}\\z"));
    return pattern.match(id).hasMatch() && !id.startsWith(u"builtin-");
}
QString filename(const QString& id) { return id + QStringLiteral(".ketscheme.json"); }
SchemeDiagnostic failure(const QString& message) {
    return {DiagnosticSeverity::Error, QStringLiteral("$catalog"), message};
}
ColorSchemeReadResult readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{}, {failure(QStringLiteral("Cannot read scheme"))}};
    // Bounded even if a file is replaced or grows after opening.
    return ColorSchemeReader::read(file.read(ColorSchemeReader::maximumBytes + 1));
}
} // namespace
ColorSchemeCatalog::ColorSchemeCatalog(QString root)
    : root_(root.isEmpty() ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                 .filePath(QStringLiteral("color-schemes"))
                           : std::move(root)) {}
QString ColorSchemeCatalog::directory() const { return root_; }
QList<ColorScheme> ColorSchemeCatalog::list() const {
    QList<ColorScheme> schemes{builtinColorScheme(SchemeKind::Light),
                               builtinColorScheme(SchemeKind::Dark)};
    const QDir directory(root_);
    for (const auto& entry : directory.entryInfoList({QStringLiteral("*.ketscheme.json")},
                                                     QDir::Files | QDir::NoSymLinks, QDir::Name)) {
        const auto result = readFile(entry.absoluteFilePath());
        if (result.ok() && validUserId(result.scheme.id) &&
            filename(result.scheme.id) == entry.fileName())
            schemes.append(result.scheme);
    }
    return schemes;
}
ColorSchemeCatalogResult ColorSchemeCatalog::load(const QString& id, SchemeKind fallback) const {
    if (id == u"builtin-light" || id == u"builtin-dark")
        return {builtinColorScheme(id == u"builtin-light" ? SchemeKind::Light : SchemeKind::Dark),
                {},
                true};
    QList<SchemeDiagnostic> diagnostics;
    const QString path = QDir(root_).filePath(filename(id));
    if (validUserId(id) && !QFileInfo(path).isSymLink()) {
        const auto result = readFile(path);
        if (result.ok() && result.scheme.id == id)
            return {result.scheme, result.diagnostics, true};
        diagnostics = result.diagnostics;
    }
    diagnostics.append({DiagnosticSeverity::Warning, QStringLiteral("$.id"),
                        QStringLiteral("Scheme unavailable; using built-in fallback")});
    return {builtinColorScheme(fallback), diagnostics, false};
}
ColorSchemeCatalogResult ColorSchemeCatalog::add(ColorScheme scheme) const {
    if (scheme.id.isEmpty())
        scheme.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ColorSchemeCatalogResult result{scheme, {}, false};
    if (!validUserId(scheme.id)) {
        result.diagnostics.append(failure(QStringLiteral("Invalid or reserved scheme ID")));
        return result;
    }
    for (auto it = scheme.ui.begin(); it != scheme.ui.end(); ++it) {
        const auto parts = it.key().split(QLatin1Char('.'));
        if (parts.size() != 2 || parts[0].isEmpty() || parts[1].isEmpty()) {
            result.diagnostics.append({DiagnosticSeverity::Error,
                                       QStringLiteral("$.ui.") + it.key(),
                                       QStringLiteral("Expected group.token UI key")});
            return result;
        }
    }
    const auto bytes = ColorSchemeWriter::write(scheme);
    const auto validated = ColorSchemeReader::read(bytes);
    result.diagnostics = validated.diagnostics;
    if (!validated.ok())
        return result;
    if (!QDir().mkpath(root_)) {
        result.diagnostics.append(failure(QStringLiteral("Cannot create scheme directory")));
        return result;
    }
    QLockFile lock(QDir(root_).filePath(QStringLiteral(".catalog.lock")));
    if (!lock.tryLock(0)) {
        result.diagnostics.append(failure(QStringLiteral("Scheme catalog is busy")));
        return result;
    }
    const QString path = QDir(root_).filePath(filename(scheme.id));
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        result.diagnostics.append(failure(QStringLiteral("Scheme ID already exists")));
        return result;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        result.diagnostics.append(failure(QStringLiteral("Cannot atomically save scheme")));
        return result;
    }
    result.scheme = validated.scheme;
    result.success = true;
    return result;
}
ColorSchemeCatalogResult ColorSchemeCatalog::duplicate(const QString& id,
                                                       const QString& name) const {
    auto original = load(id);
    if (!original.success)
        return original;
    original.scheme.id.clear();
    original.scheme.name = name;
    return add(original.scheme);
}
bool ColorSchemeCatalog::remove(const QString& id, QList<SchemeDiagnostic>* diagnostics) const {
    const auto fail = [&](const QString& message) {
        if (diagnostics)
            diagnostics->append(failure(message));
        return false;
    };
    if (!validUserId(id))
        return fail(QStringLiteral("Invalid or reserved scheme ID"));
    QLockFile lock(QDir(root_).filePath(QStringLiteral(".catalog.lock")));
    if (!lock.tryLock(0))
        return fail(QStringLiteral("Scheme catalog is busy"));
    const auto path = QDir(root_).filePath(filename(id));
    if (QFileInfo(path).isSymLink() || !QFile::remove(path))
        return fail(QStringLiteral("Cannot remove scheme"));
    return true;
}
} // namespace ketplus
