#include "ui/theme/ColorScheme.h"
#include "ui/theme/ColorSchemeTokens.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <algorithm>

// Explicit initialization pulls embedded built-ins out of the static UI library.
static void initializeSchemeResources() { Q_INIT_RESOURCE(color_schemes); }

namespace ketplus {
namespace {
using namespace scheme_detail;
QString kindName(SchemeKind kind) {
    switch (kind) {
    case SchemeKind::Light:
        return QStringLiteral("light");
    case SchemeKind::Dark:
        return QStringLiteral("dark");
    case SchemeKind::HighContrast:
        return QStringLiteral("highContrast");
    }
    return {};
}
void diagnostic(ColorSchemeReadResult& result, const QString& path, const QString& message,
                DiagnosticSeverity severity = DiagnosticSeverity::Error) {
    result.diagnostics.append({severity, path, message});
}
void unknownKeys(ColorSchemeReadResult& result, const QJsonObject& object, const QString& path) {
    for (auto it = object.begin(); it != object.end(); ++it)
        diagnostic(result, path + QLatin1Char('.') + it.key(),
                   QStringLiteral("Unknown key preserved"), DiagnosticSeverity::Warning);
}
void readColors(ColorSchemeReadResult& result, const QJsonValue& value, const QString& path,
                const QStringList& keys, SchemeColors& colors, QJsonObject& extras) {
    if (value.isUndefined())
        return;
    if (!value.isObject()) {
        diagnostic(result, path, QStringLiteral("Expected color object"));
        return;
    }
    extras = value.toObject();
    for (const auto& key : keys) {
        if (!extras.contains(key))
            continue;
        const auto entry = extras.take(key);
        const auto color =
            entry.isString() ? ColorSchemeReader::parseColor(entry.toString()) : std::nullopt;
        if (color)
            colors.insert(key, *color);
        else
            diagnostic(result, path + QLatin1Char('.') + key,
                       QStringLiteral("Expected #RGB, #RRGGBB or #RRGGBBAA"));
    }
    unknownKeys(result, extras, path);
}
QJsonObject writeColors(const SchemeColors& colors, QJsonObject object) {
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        const QColor c = it.value();
        if (!c.isValid()) {
            object.insert(it.key(), QJsonValue::Null);
            continue;
        }
        QString hex = c.name(QColor::HexRgb).toUpper();
        if (c.alpha() != 255)
            hex += QStringLiteral("%1").arg(c.alpha(), 2, 16, QLatin1Char('0')).toUpper();
        object.insert(it.key(), hex);
    }
    return object;
}
} // namespace

bool ColorSchemeReadResult::ok() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const auto& d) { return d.severity == DiagnosticSeverity::Error; });
}
std::optional<QColor> ColorSchemeReader::parseColor(const QString& hex) {
    static const QRegularExpression format(
        QStringLiteral("^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})\\z"));
    if (!format.match(hex).hasMatch())
        return std::nullopt;
    QString digits = hex.mid(1);
    if (digits.size() == 3) {
        QString expanded;
        for (const QChar c : digits) {
            expanded += c;
            expanded += c;
        }
        digits = expanded;
    }
    return QColor(digits.mid(0, 2).toInt(nullptr, 16), digits.mid(2, 2).toInt(nullptr, 16),
                  digits.mid(4, 2).toInt(nullptr, 16),
                  digits.size() == 8 ? digits.mid(6, 2).toInt(nullptr, 16) : 255);
}
ColorSchemeReadResult ColorSchemeReader::read(const QByteArray& json) {
    ColorSchemeReadResult result;
    if (json.size() > maximumBytes) {
        diagnostic(result, QStringLiteral("$"), QStringLiteral("Scheme exceeds 5 MiB"));
        return result;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        diagnostic(result, QStringLiteral("$"),
                   QStringLiteral("Expected JSON object: %1").arg(error.errorString()));
        return result;
    }
    QJsonObject object = document.object();
    const auto version = object.take(QStringLiteral("schemaVersion"));
    if (!version.isDouble() || version.toDouble() != 1)
        diagnostic(result, QStringLiteral("$.schemaVersion"),
                   QStringLiteral("Only schema version 1 is supported"));
    auto& scheme = result.scheme;
    const auto stringField = [&](const char* key, QString& output) {
        const QString name = QString::fromLatin1(key);
        const auto value = object.take(name);
        if (!value.isString() || value.toString().trimmed().isEmpty())
            diagnostic(result, QStringLiteral("$.") + name,
                       QStringLiteral("Expected nonempty string"));
        else
            output = value.toString();
    };
    stringField("id", scheme.id);
    static const QRegularExpression idFormat(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}\\z"));
    if (!idFormat.match(scheme.id).hasMatch())
        diagnostic(
            result, QStringLiteral("$.id"),
            QStringLiteral("Use 1–128 lowercase ASCII letters, digits, dot, underscore or hyphen"));
    stringField("name", scheme.name);
    QString kind;
    stringField("kind", kind);
    if (kind == u"dark")
        scheme.kind = SchemeKind::Dark;
    else if (kind == u"highContrast")
        scheme.kind = SchemeKind::HighContrast;
    else if (kind != u"light")
        diagnostic(result, QStringLiteral("$.kind"),
                   QStringLiteral("Expected light, dark or highContrast"));
    stringField("extends", scheme.extends);
    if (scheme.extends != u"builtin-light" && scheme.extends != u"builtin-dark")
        diagnostic(result, QStringLiteral("$.extends"),
                   QStringLiteral("Expected builtin-light or builtin-dark"));
    QJsonObject uiExtras;
    const auto ui = object.take(QStringLiteral("ui"));
    if (!ui.isUndefined() && !ui.isObject())
        diagnostic(result, QStringLiteral("$.ui"), QStringLiteral("Expected object"));
    else {
        uiExtras = ui.toObject();
        for (const QString& group :
             {QStringLiteral("chrome"), QStringLiteral("sidebar"), QStringLiteral("input"),
              QStringLiteral("list"), QStringLiteral("tab"), QStringLiteral("focus"),
              QStringLiteral("button")}) {
            QStringList keys;
            for (const auto& key : uiKeys)
                if (key.startsWith(group + QLatin1Char('.')))
                    keys.append(key.mid(group.size() + 1));
            SchemeColors colors;
            QJsonObject extra;
            readColors(result, uiExtras.take(group), QStringLiteral("$.ui.") + group, keys, colors,
                       extra);
            for (auto it = colors.begin(); it != colors.end(); ++it)
                scheme.ui.insert(group + QLatin1Char('.') + it.key(), it.value());
            if (!extra.isEmpty())
                uiExtras.insert(group, extra);
        }
        // Unknown groups remain intact. Known groups were checked above.
        for (auto it = uiExtras.begin(); it != uiExtras.end(); ++it)
            if (!QStringList{QStringLiteral("chrome"), QStringLiteral("sidebar"),
                             QStringLiteral("input"), QStringLiteral("list"), QStringLiteral("tab"),
                             QStringLiteral("focus"), QStringLiteral("button")}
                     .contains(it.key()))
                diagnostic(result, QStringLiteral("$.ui.") + it.key(),
                           QStringLiteral("Unknown group preserved"), DiagnosticSeverity::Warning);
    }
    if (!uiExtras.isEmpty())
        scheme.preserved.insert(QStringLiteral("ui"), uiExtras);
    for (auto group :
         {QStringLiteral("editor"), QStringLiteral("terminal"), QStringLiteral("diff")}) {
        QJsonObject extra;
        SchemeColors& target = group == u"editor"     ? scheme.editor
                               : group == u"terminal" ? scheme.terminal
                                                      : scheme.diff;
        readColors(result, object.take(group), QStringLiteral("$.") + group,
                   group == u"editor"     ? editorKeys
                   : group == u"terminal" ? terminalKeys
                                          : diffKeys,
                   target, extra);
        if (!extra.isEmpty())
            scheme.preserved.insert(group, extra);
    }
    const auto syntax = object.take(QStringLiteral("syntax"));
    QJsonObject syntaxExtras = syntax.toObject();
    if (!syntax.isUndefined() && !syntax.isObject())
        diagnostic(result, QStringLiteral("$.syntax"), QStringLiteral("Expected object"));
    for (const auto& role : syntaxKeys) {
        if (!syntaxExtras.contains(role))
            continue;
        const auto value = syntaxExtras.take(role);
        const QString path = QStringLiteral("$.syntax.") + role;
        if (!value.isObject()) {
            diagnostic(result, path, QStringLiteral("Expected style object"));
            continue;
        }
        auto styleObject = value.toObject();
        SyntaxStyle style;
        if (styleObject.contains(QStringLiteral("fg"))) {
            const auto fg = styleObject.take(QStringLiteral("fg"));
            style.fg = fg.isString() ? parseColor(fg.toString()) : std::nullopt;
            if (!style.fg)
                diagnostic(result, path + QStringLiteral(".fg"), QStringLiteral("Invalid color"));
        }
        for (auto pair : {std::pair{"bold", &style.bold}, std::pair{"italic", &style.italic},
                          std::pair{"underline", &style.underline}}) {
            const QString key = QString::fromLatin1(pair.first);
            if (!styleObject.contains(key))
                continue;
            const auto flag = styleObject.take(key);
            if (flag.isBool())
                *pair.second = flag.toBool();
            else
                diagnostic(result, path + QLatin1Char('.') + key,
                           QStringLiteral("Expected boolean"));
        }
        scheme.syntax.insert(role, style);
        unknownKeys(result, styleObject, path);
        if (!styleObject.isEmpty())
            syntaxExtras.insert(role, styleObject);
    }
    for (auto it = syntaxExtras.begin(); it != syntaxExtras.end(); ++it)
        if (!syntaxKeys.contains(it.key()))
            diagnostic(result, QStringLiteral("$.syntax.") + it.key(),
                       QStringLiteral("Unknown role preserved"), DiagnosticSeverity::Warning);
    if (!syntaxExtras.isEmpty())
        scheme.preserved.insert(QStringLiteral("syntax"), syntaxExtras);
    const auto metadata = object.take(QStringLiteral("metadata"));
    if (!metadata.isUndefined() && !metadata.isObject())
        diagnostic(result, QStringLiteral("$.metadata"), QStringLiteral("Expected object"));
    scheme.metadata = metadata.toObject();
    for (const auto& key :
         {QStringLiteral("sourceApp"), QStringLiteral("sourceFile"), QStringLiteral("license")})
        if (scheme.metadata.contains(key) && !scheme.metadata.value(key).isString())
            diagnostic(result, QStringLiteral("$.metadata.") + key,
                       QStringLiteral("Expected string"));
    unknownKeys(result, object, QStringLiteral("$"));
    for (auto it = object.begin(); it != object.end(); ++it)
        scheme.preserved.insert(it.key(), it.value());
    return result;
}
QByteArray ColorSchemeWriter::write(const ColorScheme& scheme) {
    QJsonObject object = scheme.preserved;
    object.insert(QStringLiteral("schemaVersion"), scheme.schemaVersion);
    object.insert(QStringLiteral("id"), scheme.id);
    object.insert(QStringLiteral("name"), scheme.name);
    object.insert(QStringLiteral("kind"), kindName(scheme.kind));
    object.insert(QStringLiteral("extends"), scheme.extends);
    auto ui = object.value(QStringLiteral("ui")).toObject();
    for (auto it = scheme.ui.begin(); it != scheme.ui.end(); ++it) {
        const auto parts = it.key().split(QLatin1Char('.'));
        if (parts.size() != 2)
            continue;
        ui.insert(parts[0], writeColors({{parts[1], it.value()}}, ui.value(parts[0]).toObject()));
    }
    if (!ui.isEmpty())
        object.insert(QStringLiteral("ui"), ui);
    for (auto pair : {std::pair{"editor", &scheme.editor}, std::pair{"terminal", &scheme.terminal},
                      std::pair{"diff", &scheme.diff}}) {
        const QString key = QString::fromLatin1(pair.first);
        const auto group = writeColors(*pair.second, object.value(key).toObject());
        if (!group.isEmpty())
            object.insert(key, group);
    }
    auto syntax = object.value(QStringLiteral("syntax")).toObject();
    for (auto it = scheme.syntax.begin(); it != scheme.syntax.end(); ++it) {
        auto style = syntax.value(it.key()).toObject();
        if (it->fg)
            style = writeColors({{QStringLiteral("fg"), *it->fg}}, style);
        for (auto pair : {std::pair{"bold", it->bold}, std::pair{"italic", it->italic},
                          std::pair{"underline", it->underline}})
            if (pair.second)
                style.insert(QString::fromLatin1(pair.first), *pair.second);
        syntax.insert(it.key(), style);
    }
    if (!syntax.isEmpty())
        object.insert(QStringLiteral("syntax"), syntax);
    if (!scheme.metadata.isEmpty())
        object.insert(QStringLiteral("metadata"), scheme.metadata);
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}
ColorScheme builtinColorScheme(SchemeKind kind) {
    static const bool initialized = [] {
        initializeSchemeResources();
        return true;
    }();
    Q_UNUSED(initialized);
    const auto load = [](const QString& name) {
        QFile file(QStringLiteral(":/color-schemes/") + name + QStringLiteral(".ketscheme.json"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Missing embedded color scheme");
        const auto result = ColorSchemeReader::read(file.readAll());
        if (!result.ok())
            qFatal("Invalid embedded color scheme");
        return result.scheme;
    };
    static const auto light = load(QStringLiteral("light"));
    static const auto dark = load(QStringLiteral("dark"));
    return kind == SchemeKind::Light ? light : dark;
}
ColorScheme resolveColorScheme(const ColorScheme& scheme) {
    auto resolved = builtinColorScheme(scheme.extends == u"builtin-dark" ? SchemeKind::Dark
                                                                         : SchemeKind::Light);
    resolved.id = scheme.id;
    resolved.name = scheme.name;
    resolved.kind = scheme.kind;
    resolved.extends = scheme.extends;
    resolved.metadata = scheme.metadata;
    resolved.preserved = scheme.preserved;
    for (auto pair :
         {std::pair{&resolved.ui, &scheme.ui}, std::pair{&resolved.editor, &scheme.editor},
          std::pair{&resolved.terminal, &scheme.terminal}, std::pair{&resolved.diff, &scheme.diff}})
        for (auto it = pair.second->begin(); it != pair.second->end(); ++it)
            if (it.value().isValid())
                pair.first->insert(it.key(), it.value());
    for (auto it = scheme.syntax.begin(); it != scheme.syntax.end(); ++it) {
        if (!resolved.syntax.contains(it.key()))
            continue;
        auto& style = resolved.syntax[it.key()];
        if (it->fg && it->fg->isValid())
            style.fg = it->fg;
        if (it->bold)
            style.bold = it->bold;
        if (it->italic)
            style.italic = it->italic;
        if (it->underline)
            style.underline = it->underline;
    }
    return resolved;
}
ThemePalette colorSchemePalette(const ColorScheme& scheme) {
    const auto resolved = resolveColorScheme(scheme);
    ThemePalette palette;
    palette.dark =
        resolved.kind == SchemeKind::Dark ||
        (resolved.kind == SchemeKind::HighContrast && resolved.extends == u"builtin-dark");
    const auto hex = [](const QColor& color) { return color.name(QColor::HexRgb).toUpper(); };
    for (const auto& token : scheme_detail::paletteTokens)
        palette.*(token.member) = hex(resolved.ui.value(QString::fromLatin1(token.path)));
    palette.syntaxKeyword = hex(*resolved.syntax.value(QStringLiteral("keyword")).fg);
    palette.syntaxString = hex(*resolved.syntax.value(QStringLiteral("string")).fg);
    palette.syntaxNumber = hex(*resolved.syntax.value(QStringLiteral("number")).fg);
    palette.syntaxType = hex(*resolved.syntax.value(QStringLiteral("type")).fg);
    palette.syntaxComment = hex(*resolved.syntax.value(QStringLiteral("comment")).fg);
    palette.syntaxAttribute = hex(*resolved.syntax.value(QStringLiteral("attribute")).fg);
    return palette;
}
} // namespace ketplus
