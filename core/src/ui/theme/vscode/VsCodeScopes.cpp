#include "VsCodeImport_p.h"
#include "VsCodeMapping.h"
#include <QSet>

namespace ketplus::vscode_detail {
namespace {
QString roleFor(const QString& scope) {
    for (const auto c : scope) {
        if (!((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') ||
              c == u'.' || c == u'-' || c == u'_'))
            return {};
    }
    for (const auto& mapping : scopeMappings) {
        const auto prefix = QLatin1String(mapping.scope);
        if (scope == prefix || scope.startsWith(QString(prefix) + '.'))
            return QLatin1String(mapping.role);
    }
    return {};
}
void applySettings(const QJsonObject& settings, const QString& role, ColorScheme& scheme,
                   ImportReport& out, const QString& source) {
    auto& style = scheme.syntax[role];
    if (settings.contains(QStringLiteral("foreground"))) {
        const auto fg = parseColor(settings.value(QStringLiteral("foreground")));
        if (fg)
            style.fg = *fg;
        else
            report(out, ImportDisposition::Skipped, source + QStringLiteral("/foreground"), {},
                   QStringLiteral("Invalid syntax foreground; previous/default color retained"));
    }
    if (settings.contains(QStringLiteral("fontStyle"))) {
        const auto font = settings.value(QStringLiteral("fontStyle"));
        if (font.isString()) {
            style.bold = false;
            style.italic = false;
            style.underline = false;
            auto text = font.toString();
            for (auto& c : text)
                if (c.isSpace())
                    c = u' ';
            for (const auto& flag : text.split(' ', Qt::SkipEmptyParts)) {
                if (flag == u"bold")
                    style.bold = true;
                else if (flag == u"italic")
                    style.italic = true;
                else if (flag == u"underline")
                    style.underline = true;
                else
                    report(out, ImportDisposition::Skipped,
                           source + QStringLiteral("/fontStyle/") + flag, {},
                           QStringLiteral(
                               "Unsupported font effect (including strikethrough) omitted"));
            }
        } else
            report(out, ImportDisposition::Skipped, source + QStringLiteral("/fontStyle"), {},
                   QStringLiteral("fontStyle must be a string; previous flags retained"));
    }
    for (auto it = settings.begin(); it != settings.end(); ++it)
        if (it.key() != u"foreground" && it.key() != u"fontStyle")
            report(out, ImportDisposition::Skipped, source + '/' + it.key(), {},
                   QStringLiteral("Syntax background or setting has no shared-role equivalent"));
    report(out, ImportDisposition::Approximated, source, QStringLiteral("syntax.") + role,
           QStringLiteral("Scope collapsed to a shared Lexilla role; later explicit fields win "
                          "without a TextMate scope stack"));
}
} // namespace
void mapGlobalColors(const QList<Rule>& rules, AuthoredColors& colors,
                     const QSet<QString>& declared, ImportReport& out) {
    const QMap<QString, QString> aliases{
        {"foreground", "editor.foreground"},
        {"background", "editor.background"},
        {"caret", "editorCursor.foreground"},
        {"selection", "editor.selectionBackground"},
        {"lineHighlight", "editor.lineHighlightBackground"},
        {"gutterForeground", "editorLineNumber.foreground"},
        {"invisibles", "editorWhitespace.foreground"},
        {"guide", "editorIndentGuide.background"},
        {"inactiveSelection", "editor.inactiveSelectionBackground"}};
    const QStringList ansi{
        "ansiBlack",       "ansiRed",           "ansiGreen",       "ansiYellow",
        "ansiBlue",        "ansiMagenta",       "ansiCyan",        "ansiWhite",
        "ansiBrightBlack", "ansiBrightRed",     "ansiBrightGreen", "ansiBrightYellow",
        "ansiBrightBlue",  "ansiBrightMagenta", "ansiBrightCyan",  "ansiBrightWhite"};
    for (const auto& rule : rules) {
        const auto scope = rule.value.value(QStringLiteral("scope"));
        if (!scope.isUndefined() && !(scope.isString() && scope.toString().trimmed().isEmpty()))
            continue;
        const auto settings = rule.value.value(QStringLiteral("settings")).toObject();
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            const auto source = rule.source + '/' + it.key();
            const auto id = ansi.contains(it.key()) ? QStringLiteral("terminal.") + it.key()
                                                    : aliases.value(it.key());
            if (id.isEmpty()) {
                report(out, ImportDisposition::Skipped, source, {},
                       it.key() == u"fontStyle"
                           ? QStringLiteral(
                                 "Global token typography has no shared default-text style slot")
                           : QStringLiteral(
                                 "Global TextMate setting has no supported color conversion"));
                continue;
            }
            const auto parsed = parseColor(it.value());
            if (!parsed || declared.contains(id)) {
                report(out, ImportDisposition::Skipped, source, id,
                       !parsed ? QStringLiteral("Invalid global TextMate color")
                               : QStringLiteral(
                                     "Explicit workbench color declaration takes precedence"));
                continue;
            }
            colors[id] = {*parsed, source};
            report(out, ImportDisposition::Approximated, source, id,
                   QStringLiteral(
                       "Global TextMate color converted before workbench fallback resolution"));
        }
    }
}
void mapRules(const QList<Rule>& rules, ColorScheme& scheme, ImportReport& out) {
    for (const auto& rule : rules) {
        const auto rawSettings = rule.value.value(QStringLiteral("settings"));
        if (!rawSettings.isObject()) {
            report(out, ImportDisposition::Skipped, rule.source, {},
                   QStringLiteral("Token rule settings must be an object"));
            continue;
        }
        const auto settings = rawSettings.toObject();
        const auto selector = rule.value.value(QStringLiteral("scope"));
        if (selector.isUndefined() ||
            (selector.isString() && selector.toString().trimmed().isEmpty()))
            continue;
        QStringList selectors;
        if (selector.isString())
            selectors = selector.toString().split(',');
        else if (selector.isArray()) {
            for (const auto& value : selector.toArray()) {
                if (value.isString())
                    selectors.append(value.toString().split(','));
                else
                    report(out, ImportDisposition::Skipped, rule.source + QStringLiteral("/scope"),
                           {}, QStringLiteral("Scope array member must be a string"));
            }
        } else {
            report(out, ImportDisposition::Skipped, rule.source + QStringLiteral("/scope"), {},
                   QStringLiteral("Scope must be a string or array"));
            continue;
        }
        QStringList roles;
        for (const auto& raw : selectors) {
            const auto scope = raw.trimmed();
            const auto role = roleFor(scope);
            if (role.isEmpty())
                report(out, ImportDisposition::Skipped,
                       rule.source + QStringLiteral("/scope/") + scope, {},
                       QStringLiteral("Unsupported selector; only documented single dotted scope "
                                      "prefixes are recognized"));
            else if (!roles.contains(role))
                roles.append(role);
        }
        // Deduplicate comma selectors targeting the same role. Different
        // roles receive identical settings; ordering between rules is unchanged.
        for (const auto& role : roles)
            applySettings(settings, role, scheme, out, rule.source);
    }
}
} // namespace ketplus::vscode_detail
