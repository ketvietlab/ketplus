#include "VsCodeImport_p.h"
#include <QFileInfo>

namespace ketplus::vscode_detail {
bool Session::read(const QString& path, QByteArray& bytes, ImportReport& out) {
    if (!remaining) {
        failure(out, path, QStringLiteral("Import read budget exhausted"));
        return false;
    }
    ket::theme::io::Error error;
    if (!resolver.read(path, &bytes, &error)) {
        failure(out, path, error.message);
        return false;
    }
    if (quint64(bytes.size()) > remaining) {
        remaining = 0;
        bytes.clear();
        failure(out, path, QStringLiteral("Import exceeds 100 MiB cumulative read budget"));
        return false;
    }
    remaining -= quint64(bytes.size());
    return true;
}
bool Session::convert(const QString& path, const QString& label, const QString& hint,
                      VsCodeImportedScheme& variant) {
    auto& out = variant.report;
    if (!remaining) {
        failure(out, path, QStringLiteral("Import read budget exhausted"));
        return false;
    }
    QVector<ket::theme::io::JsonSource> chain;
    ket::theme::io::Error error;
    if (!resolver.loadJsonChain(path, &chain, &error, &remaining)) {
        failure(out, error.source.isEmpty() ? path : error.source,
                QStringLiteral("%1 (line %2, column %3)")
                    .arg(error.message)
                    .arg(error.line)
                    .arg(error.column));
        return false;
    }
    variant.sourcePath = path;
    auto& scheme = variant.scheme;
    const auto root = chain.last().document.object();
    scheme.name =
        !label.trimmed().isEmpty() ? label : root.value(QStringLiteral("name")).toString();
    if (scheme.name.startsWith('%') && scheme.name.endsWith('%')) {
        report(
            out, ImportDisposition::Approximated, path + QStringLiteral("/name"),
            QStringLiteral("name"),
            QStringLiteral("Localized label is not resolved; theme name or source filename used"));
        scheme.name = root.value(QStringLiteral("name")).toString();
        if (scheme.name.startsWith('%') && scheme.name.endsWith('%'))
            scheme.name.clear();
    }
    if (scheme.name.trimmed().isEmpty()) {
        scheme.name = QFileInfo(path).completeBaseName();
        report(out, ImportDisposition::Approximated, path + QStringLiteral("/name"),
               QStringLiteral("name"),
               QStringLiteral("Missing name; source filename supplies display name"));
    }
    scheme.metadata = {{QStringLiteral("sourceApp"), QStringLiteral("VS Code")},
                       {QStringLiteral("sourceFile"), path}};
    QCryptographicHash identity(QCryptographicHash::Sha256);
    identity.addData(path.toUtf8());
    identity.addData(label.toUtf8());
    identity.addData(hint.toUtf8());
    AuthoredColors colors;
    QList<Rule> rules;
    QString declaredType;
    QJsonObject semantic;
    for (const auto& layer : chain) {
        const auto object = layer.document.object();
        identity.addData(layer.path.toUtf8());
        identity.addData(layer.document.toJson(QJsonDocument::Compact));
        if (object.contains(QStringLiteral("type")))
            declaredType = object.value(QStringLiteral("type")).toString();
        if (object.contains(QStringLiteral("colors"))) {
            const auto value = object.value(QStringLiteral("colors"));
            if (!value.isObject()) {
                failure(out, layer.path + QStringLiteral("/colors"),
                        QStringLiteral("Expected colors object"));
                return false;
            }
            const auto entries = value.toObject();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                const auto source = layer.path + QStringLiteral("/colors/") + it.key();
                if (it.value().isNull()) {
                    colors.remove(it.key());
                    report(
                        out, ImportDisposition::Mapped, source, {},
                        QStringLiteral(
                            "Explicit null removes the inherited color; fallback chain applies"));
                } else if (const auto parsed = parseColor(it.value()))
                    colors[it.key()] = {*parsed, source};
                else
                    report(out, ImportDisposition::Skipped, source, {},
                           QStringLiteral(
                               "Invalid VS Code hex color; previous/default value retained"));
            }
        }
        const auto tokens = object.value(QStringLiteral("tokenColors"));
        if (tokens.isArray()) {
            const auto array = tokens.toArray();
            for (qsizetype i = 0; i < array.size(); ++i) {
                const auto source =
                    layer.path + QStringLiteral("/tokenColors/") + QString::number(i);
                if (!array[i].isObject()) {
                    failure(out, source, QStringLiteral("Token rule must be an object"));
                    return false;
                }
                rules.append({array[i].toObject(), source});
            }
        } else if (tokens.isString()) {
            QString resource;
            if (!resolver.resolve(layer.path, tokens.toString(), &resource, &error)) {
                failure(out, layer.path + QStringLiteral("/tokenColors"), error.message);
                return false;
            }
            QByteArray bytes;
            if (!read(resource, bytes, out) || !plistRules(bytes, resource, rules, out))
                return false;
            identity.addData(resource.toUtf8());
            identity.addData(bytes);
        } else if (!tokens.isUndefined() && !tokens.isNull()) {
            failure(out, layer.path + QStringLiteral("/tokenColors"),
                    QStringLiteral("Expected rule array or relative tmTheme path"));
            return false;
        }
        if (object.contains(QStringLiteral("semanticTokenColors"))) {
            const auto value = object.value(QStringLiteral("semanticTokenColors"));
            if (value.isObject()) {
                const auto entries = value.toObject();
                for (auto it = entries.begin(); it != entries.end(); ++it)
                    semantic.insert(it.key(), it.value());
            } else
                scheme.metadata.insert(QStringLiteral("vscodeUnsupportedSemanticTokenColors"),
                                       value);
            report(out, ImportDisposition::Skipped,
                   layer.path + QStringLiteral("/semanticTokenColors"),
                   QStringLiteral("metadata.vscodeSemanticTokenColors"),
                   QStringLiteral(
                       "Preserved as inert metadata only; semantic highlighting is not applied"));
        }
        if (object.contains(QStringLiteral("semanticHighlighting"))) {
            scheme.metadata.insert(QStringLiteral("vscodeSemanticHighlighting"),
                                   object.value(QStringLiteral("semanticHighlighting")));
            report(out, ImportDisposition::Skipped,
                   layer.path + QStringLiteral("/semanticHighlighting"), QStringLiteral("metadata"),
                   QStringLiteral("Declaration preserved without enabling semantic highlighting"));
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.key() != u"name" && it.key() != u"type" && it.key() != u"include" &&
                it.key() != u"colors" && it.key() != u"tokenColors" &&
                it.key() != u"semanticTokenColors" && it.key() != u"semanticHighlighting")
                report(out, ImportDisposition::Skipped, layer.path + '/' + it.key(), {},
                       QStringLiteral("Theme metadata/property has no supported KetPlus behavior"));
        }
    }
    if (!semantic.isEmpty())
        scheme.metadata.insert(QStringLiteral("vscodeSemanticTokenColors"), semantic);
    mapWorkbench(colors, scheme, out);
    mapRules(rules, scheme, out);
    const auto kind = hint.isEmpty() ? declaredType : hint;
    bool dark = true;
    bool declaredKind = true;
    if (kind == u"vs" || kind == u"light") {
        dark = false;
        scheme.kind = SchemeKind::Light;
    } else if (kind == u"vs-dark" || kind == u"dark")
        scheme.kind = SchemeKind::Dark;
    else if (kind == u"hc-black" || kind == u"hcDark")
        scheme.kind = SchemeKind::HighContrast;
    else if (kind == u"hc-light" || kind == u"hcLight") {
        dark = false;
        scheme.kind = SchemeKind::HighContrast;
    } else {
        declaredKind = false;
        const auto background = scheme.editor.constFind(QStringLiteral("bg"));
        dark = background == scheme.editor.cend() || background->alpha() != 255 ||
               qGray(background->rgb()) < 128;
        scheme.kind = dark ? SchemeKind::Dark : SchemeKind::Light;
        report(out, ImportDisposition::Approximated, path + QStringLiteral("/type"),
               QStringLiteral("kind"),
               QStringLiteral(
                   "Missing/unknown kind inferred from opaque editor background, otherwise dark"));
    }
    scheme.extends = dark ? QStringLiteral("builtin-dark") : QStringLiteral("builtin-light");
    if (!hint.isEmpty())
        report(out, declaredKind ? ImportDisposition::Mapped : ImportDisposition::Approximated,
               path + QStringLiteral("/uiTheme"), QStringLiteral("kind"),
               declaredKind
                   ? QStringLiteral("Manifest uiTheme determines declared variant kind")
                   : QStringLiteral("Unknown manifest uiTheme; kind inferred from theme colors"));
    scheme.id = QStringLiteral("vscode-") + QString::fromLatin1(identity.result().toHex().left(32));
    report(out, ImportDisposition::Approximated, path, QStringLiteral("syntax"),
           QStringLiteral("Lexilla uses shared roles, not TextMate scope stacks; rule order is "
                          "parent first then child, later explicit fields override"));
    report(out, ImportDisposition::Skipped, path, {},
           QStringLiteral("Extension JavaScript, grammars, semantic token providers and UI "
                          "contributions are never executed"));
    return !failed(out);
}
} // namespace ketplus::vscode_detail
