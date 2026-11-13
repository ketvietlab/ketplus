#include "JetBrainsImport_p.h"
#include "JetBrainsMapping.h"
#include <QJsonObject>
#include <QSet>
#include <functional>

namespace ketplus::jetbrains_detail {
bool parseTheme(Source& source, const QByteArray& bytes, const QString& path,
                JetBrainsImportedScheme& output, QString* editorPath) {
    ket::theme::io::Error ioError;
    const auto document = ket::theme::io::parseJsonc(bytes, &ioError);
    if (!document.isObject()) {
        error(output.report, path,
              document.isNull() ? QStringLiteral("%1 (line %2, column %3)")
                                      .arg(ioError.message)
                                      .arg(ioError.line)
                                      .arg(ioError.column)
                                : QStringLiteral("Theme root must be an object"));
        return false;
    }
    const auto object = document.object();
    output.sourcePath = path;
    output.scheme = newScheme(path, bytes, object.value(QStringLiteral("name")).toString());
    if (!object.value(QStringLiteral("name")).isString() ||
        object.value(QStringLiteral("name")).toString().trimmed().isEmpty())
        item(output.report, ImportDisposition::Approximated, path + QStringLiteral("/name"),
             QStringLiteral("name"),
             QStringLiteral(
                 "Missing or invalid theme name; source filename supplies the display name"));
    for (const auto& key : {QStringLiteral("ui"), QStringLiteral("colors")}) {
        if (object.contains(key) && !object.value(key).isObject()) {
            error(output.report, path + '/' + key, QStringLiteral("Expected object"));
            return false;
        }
    }
    if (object.contains(QStringLiteral("dark")) && !object.value(QStringLiteral("dark")).isBool()) {
        error(output.report, path + QStringLiteral("/dark"), QStringLiteral("Expected boolean"));
        return false;
    }
    if (object.contains(QStringLiteral("editorScheme"))) {
        const auto reference = object.value(QStringLiteral("editorScheme"));
        if (!reference.isString()) {
            error(output.report, path + QStringLiteral("/editorScheme"),
                  QStringLiteral("Expected local resource path string"));
            return false;
        }
        auto resource = reference.toString();
        QString declaring = path;
        if (resource.startsWith('/') && !resource.startsWith(QStringLiteral("//"))) {
            resource.remove(0, 1);
            declaring.clear();
        }
        QString resolved;
        if (!source.resolver.resolve(declaring, resource, &resolved, &ioError)) {
            error(output.report, path + QStringLiteral("/editorScheme"), ioError.message);
            return false;
        }
        QByteArray editorBytes;
        if (!source.read(resolved, editorBytes, output.report))
            return false;
        JetBrainsImportedScheme editor;
        if (!parseEditor(editorBytes, resolved, editor)) {
            output.report.diagnostics.append(editor.report.diagnostics);
            return false;
        }
        output.scheme.editor = editor.scheme.editor;
        output.scheme.syntax = editor.scheme.syntax;
        output.scheme.terminal = editor.scheme.terminal;
        output.scheme.diff = editor.scheme.diff;
        output.scheme.kind = editor.scheme.kind;
        output.scheme.extends = editor.scheme.extends;
        output.scheme.metadata.insert(
            QStringLiteral("jetBrainsParentScheme"),
            editor.scheme.metadata.value(QStringLiteral("jetBrainsParentScheme")));
        output.scheme.metadata.insert(QStringLiteral("jetBrainsEditorScheme"), resolved);
        output.report.items.append(editor.report.items);
        output.report.diagnostics.append(editor.report.diagnostics);
        if (editorPath)
            *editorPath = resolved;
        // Include referenced content in identity, without including machine paths.
        output.scheme.id = newScheme(path, bytes + editorBytes, output.scheme.name).id;
        item(output.report, ImportDisposition::Mapped, path + QStringLiteral("/editorScheme"),
             resolved, QStringLiteral("Bundled editor scheme merged with UI theme"));
    }
    if (object.contains(QStringLiteral("dark"))) {
        const bool dark = object.value(QStringLiteral("dark")).toBool();
        if ((output.scheme.kind == SchemeKind::Dark) != dark && !output.scheme.editor.isEmpty())
            item(output.report, ImportDisposition::Approximated, path + QStringLiteral("/dark"),
                 QStringLiteral("kind"),
                 QStringLiteral("UI dark declaration overrides inferred editor kind"));
        setKind(output.scheme, dark);
        item(output.report, ImportDisposition::Mapped, path + QStringLiteral("/dark"),
             QStringLiteral("kind"), QStringLiteral("Explicit UI theme kind selects KetPlus base"));
    } else
        item(output.report, ImportDisposition::Approximated, path + QStringLiteral("/dark"),
             QStringLiteral("kind"),
             QStringLiteral("Missing dark flag; editor inference or KetPlus light base used"));
    item(
        output.report, ImportDisposition::Approximated, path, QStringLiteral("extends"),
        QStringLiteral(
            "IDE UI base definitions are unavailable; KetPlus built-in base supplies unmapped UI"));
    const auto palette = object.value(QStringLiteral("colors")).toObject();
    QSet<QString> usedColors;
    std::function<std::optional<QColor>(const QString&, QSet<QString>, int)> resolveColor =
        [&](const QString& value, QSet<QString> active, int depth) -> std::optional<QColor> {
        if (depth > ket::theme::io::MaxIncludeLevels)
            return {};
        if (palette.contains(value)) {
            if (active.contains(value) || !palette.value(value).isString())
                return {};
            active.insert(value);
            const auto resolved = resolveColor(palette.value(value).toString(), active, depth + 1);
            if (resolved)
                usedColors.insert(value);
            return resolved;
        }
        return color(value, false);
    };
    std::function<void(const QJsonObject&, const QString&)> mapUi = [&](const QJsonObject& ui,
                                                                        const QString& prefix) {
        for (auto it = ui.begin(); it != ui.end(); ++it) {
            const auto key = prefix.isEmpty() ? it.key() : prefix + '.' + it.key();
            const auto sourcePath = path + QStringLiteral("/ui/") + key;
            if (it.value().isObject()) {
                if (key.count('.') >= 32)
                    item(output.report, ImportDisposition::Skipped, sourcePath, {},
                         QStringLiteral("UI nesting exceeds 32 levels"));
                else
                    mapUi(it.value().toObject(), key);
                continue;
            }
            QString target;
            for (const auto& mapping : uiColors)
                if (key == QLatin1String(mapping.source)) {
                    target = QLatin1String(mapping.target);
                    break;
                }
            if (target.isEmpty()) {
                item(output.report, ImportDisposition::Skipped, sourcePath, {},
                     QStringLiteral("No corresponding KetPlus component; "
                                    "wildcard/platform/geometry/font rules are not applied"));
                continue;
            }
            const auto parsed = it.value().isString() ? resolveColor(it.value().toString(), {}, 0)
                                                      : std::optional<QColor>{};
            if (!parsed) {
                item(output.report, ImportDisposition::Skipped, sourcePath, target,
                     QStringLiteral("Invalid color or unresolved/cyclic/deep named palette "
                                    "reference; fallback retained"));
                continue;
            }
            setColor(output.scheme, target, *parsed);
            item(output.report, ImportDisposition::Approximated, sourcePath, target,
                 QStringLiteral("JetBrains component color mapped to nearest KetPlus component; UI "
                                "engines differ"));
        }
    };
    mapUi(object.value(QStringLiteral("ui")).toObject(), {});
    for (auto it = palette.begin(); it != palette.end(); ++it)
        item(
            output.report,
            usedColors.contains(it.key()) ? ImportDisposition::Mapped : ImportDisposition::Skipped,
            path + QStringLiteral("/colors/") + it.key(), {},
            usedColors.contains(it.key())
                ? QStringLiteral("Named color resolved by a mapped UI key")
                : QStringLiteral("Unused or unresolved palette entry has no direct KetPlus token"));
    const QSet<QString> known{QStringLiteral("name"), QStringLiteral("dark"),
                              QStringLiteral("colors"), QStringLiteral("ui"),
                              QStringLiteral("editorScheme")};
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!known.contains(it.key()))
            item(output.report, ImportDisposition::Skipped, path + '/' + it.key(), {},
                 QStringLiteral("IDE metadata, icons, backgrounds, inheritance and plugin behavior "
                                "are not executed or applied"));
    return !hasErrors(output.report);
}
} // namespace ketplus::jetbrains_detail
