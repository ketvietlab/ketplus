#include "JetBrainsImport_p.h"
#include "JetBrainsMapping.h"
#include <QSet>

namespace ketplus::jetbrains_detail {
using ket::theme::io::XmlNode;
namespace {
QString optionName(const XmlNode& node) { return node.attributes.value(QStringLiteral("name")); }
const SyntaxMapping* syntaxMapping(const QString& name) {
    for (const auto& mapping : syntaxMappings)
        if (name == QLatin1String(mapping.source))
            return &mapping;
    return nullptr;
}
QString attributeTarget(const QString& name) {
    for (const auto& mapping : attributeColors)
        if (name == QLatin1String(mapping.source))
            return QLatin1String(mapping.target);
    return {};
}
void mapColor(JetBrainsImportedScheme& output, const QString& source, const QString& target,
              const QString& value) {
    const auto parsed = color(value, true);
    if (!parsed) {
        item(output.report, ImportDisposition::Skipped, source, target,
             QStringLiteral("Invalid or empty XML color; KetPlus fallback retained"));
        return;
    }
    setColor(output.scheme, target, *parsed);
    item(output.report, ImportDisposition::Mapped, source, target,
         QStringLiteral("Explicit JetBrains color mapping"));
}
void attribute(JetBrainsImportedScheme& output, const XmlNode& node, const QString& path) {
    const auto name = optionName(node);
    const auto source = path + QStringLiteral("/attributes/") + name;
    const auto* mapping = syntaxMapping(name);
    const auto role = mapping ? QLatin1String(mapping->role) : QString();
    if (mapping && mapping->languageSpecific) {
        if (output.scheme.syntax.contains(role)) {
            item(output.report, ImportDisposition::Skipped, source,
                 QStringLiteral("syntax.") + role,
                 QStringLiteral(
                     "Language-specific override omitted because a shared role already exists"));
            return;
        }
        item(output.report, ImportDisposition::Approximated, source,
             QStringLiteral("syntax.") + role,
             QStringLiteral("Language-specific styling collapsed into a shared syntax role"));
    }
    const XmlNode* value = nullptr;
    for (const auto& child : node.children)
        if (child.name == u"value")
            value = &child;
    if (!value) {
        item(output.report, ImportDisposition::Skipped, source, {},
             QStringLiteral("No authored value; IDE baseAttributes inheritance unavailable, "
                            "KetPlus base retained"));
        return;
    }
    SyntaxStyle style;
    if (mapping) {
        style.bold = false;
        style.italic = false;
        style.underline = false;
    }
    bool recognized = mapping != nullptr;
    for (const auto& field : value->children) {
        const auto key = optionName(field);
        const auto fieldSource = source + '/' + key;
        const auto text = field.attributes.value(QStringLiteral("value"));
        const auto target = attributeTarget(name + '.' + key);
        if (!target.isEmpty()) {
            mapColor(output, fieldSource, target, text);
            recognized = true;
        } else if (mapping && key == u"FOREGROUND") {
            const auto parsed = color(text, true);
            if (parsed) {
                style.fg = *parsed;
                item(output.report, ImportDisposition::Mapped, fieldSource,
                     QStringLiteral("syntax.") + role + QStringLiteral(".fg"),
                     QStringLiteral("Shared syntax foreground"));
            } else
                item(output.report, ImportDisposition::Skipped, fieldSource, {},
                     QStringLiteral("Invalid or empty color; fallback retained"));
        } else if (mapping && key == u"FONT_TYPE") {
            bool valid = false;
            const auto font = text.toInt(&valid);
            if (valid && font >= 0 && font <= 3) {
                style.bold = bool(font & 1);
                style.italic = bool(font & 2);
                item(output.report, ImportDisposition::Mapped, fieldSource,
                     QStringLiteral("syntax.") + role,
                     QStringLiteral("FONT_TYPE bits become explicit bold/italic flags"));
            } else
                item(output.report, ImportDisposition::Skipped, fieldSource, {},
                     QStringLiteral("Invalid FONT_TYPE; plain flags retained"));
        } else if (mapping && key == u"EFFECT_TYPE") {
            bool valid = false;
            const int effect = text.toInt(&valid);
            if (valid && (effect == 1 || effect == 2 || effect == 4 || effect == 5)) {
                style.underline = true;
                item(output.report,
                     effect == 1 ? ImportDisposition::Mapped : ImportDisposition::Approximated,
                     fieldSource, QStringLiteral("syntax.") + role + QStringLiteral(".underline"),
                     QStringLiteral(
                         "Line/wave/bold/dotted effect represented by a plain underline"));
            } else if (valid && effect == -1) {
                item(output.report, ImportDisposition::Mapped, fieldSource,
                     QStringLiteral("syntax.") + role + QStringLiteral(".underline"),
                     QStringLiteral("No effect becomes underline=false"));
            } else
                item(output.report, ImportDisposition::Skipped, fieldSource, {},
                     QStringLiteral("Border, strikeout or unknown effect has no equivalent"));
        } else {
            QString reason = QStringLiteral("No equivalent KetPlus attribute property");
            if (key == u"BACKGROUND")
                reason = QStringLiteral("Syntax backgrounds have no shared-role equivalent");
            else if (key.contains(u"FONT"))
                reason = QStringLiteral("Scheme font family/size or default font styling is "
                                        "ignored; user typography retained");
            else if (key.startsWith(u"EFFECT"))
                reason = QStringLiteral("Effect color/geometry is unsupported");
            item(output.report, ImportDisposition::Skipped, fieldSource, {}, reason);
        }
    }
    if (mapping) {
        auto& existing = output.scheme.syntax[role];
        if (existing.fg || existing.bold || existing.italic || existing.underline)
            item(output.report, ImportDisposition::Approximated, source,
                 QStringLiteral("syntax.") + role,
                 QStringLiteral(
                     "Multiple IDE attributes share one role; later authored fields override"));
        if (style.fg)
            existing.fg = style.fg;
        existing.bold = style.bold;
        existing.italic = style.italic;
        existing.underline = style.underline;
    }
    if (!recognized)
        item(output.report, ImportDisposition::Skipped, source, {},
             QStringLiteral("IDE/language-specific key has no explicit shared mapping"));
}
} // namespace
bool parseEditor(const QByteArray& bytes, const QString& path, JetBrainsImportedScheme& output) {
    ket::theme::io::Error ioError;
    XmlNode root;
    if (!ket::theme::io::parseXml(bytes, &root, &ioError)) {
        error(output.report, path,
              QStringLiteral("%1 (line %2, column %3)")
                  .arg(ioError.message)
                  .arg(ioError.line)
                  .arg(ioError.column));
        return false;
    }
    if (root.name != u"scheme") {
        error(output.report, path, QStringLiteral("Expected JetBrains <scheme> root"));
        return false;
    }
    output.sourcePath = path;
    output.scheme = newScheme(path, bytes, root.attributes.value(QStringLiteral("name")));
    if (root.attributes.value(QStringLiteral("name")).trimmed().isEmpty())
        item(output.report, ImportDisposition::Approximated, path + QStringLiteral("/@name"),
             QStringLiteral("name"),
             QStringLiteral("Missing scheme name; source filename supplies the display name"));
    const auto parent = root.attributes.value(QStringLiteral("parent_scheme"));
    setKind(output.scheme, parent.contains(u"darcula", Qt::CaseInsensitive) ||
                               parent.contains(u"dark", Qt::CaseInsensitive));
    output.scheme.metadata.insert(QStringLiteral("jetBrainsParentScheme"), parent);
    item(output.report, ImportDisposition::Approximated, path + QStringLiteral("/@parent_scheme"),
         QStringLiteral("extends"),
         QStringLiteral(
             "IDE parent '%1' is unavailable; approximate KetPlus light/dark base replaces it")
             .arg(parent));
    QList<XmlNode> attributes;
    for (const auto& section : root.children) {
        if (section.name == u"colors") {
            for (const auto& entry : section.children) {
                const auto name = optionName(entry);
                bool found = false;
                for (const auto& mapping : editorColors)
                    if (name == QLatin1String(mapping.source)) {
                        mapColor(output, path + QStringLiteral("/colors/") + name,
                                 QLatin1String(mapping.target),
                                 entry.attributes.value(QStringLiteral("value")));
                        found = true;
                        break;
                    }
                if (!found)
                    item(output.report, ImportDisposition::Skipped,
                         path + QStringLiteral("/colors/") + name, {},
                         QStringLiteral("IDE color has no equivalent KetPlus token"));
            }
        } else if (section.name == u"attributes")
            attributes.append(section.children);
        else
            item(output.report, ImportDisposition::Skipped,
                 path + '/' + section.name + '/' + optionName(section), {},
                 section.name == u"option"
                     ? QStringLiteral(
                           "Scheme font/size and IDE options are ignored; user typography retained")
                     : QStringLiteral("IDE metadata/section is not applied"));
    }
    // Shared DEFAULT_* roles take precedence over language-specific fallbacks.
    for (bool languageSpecific : {false, true})
        for (const auto& entry : attributes) {
            const auto* mapping = syntaxMapping(optionName(entry));
            if (bool(mapping && mapping->languageSpecific) == languageSpecific)
                attribute(output, entry, path);
        }
    const auto background = output.scheme.editor.constFind(QStringLiteral("bg"));
    if (background != output.scheme.editor.cend() && background->alpha() == 255)
        setKind(output.scheme, qGray(background->rgb()) < 128);
    item(output.report, ImportDisposition::Approximated, path, QStringLiteral("kind"),
         QStringLiteral("Kind inferred from opaque editor background brightness, otherwise parent "
                        "name (Darcula/dark); remaining bases use light"));
    return true;
}
} // namespace ketplus::jetbrains_detail
