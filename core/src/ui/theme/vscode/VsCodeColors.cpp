#include "VsCodeImport_p.h"
#include "VsCodeMapping.h"
#include <QSet>
#include <algorithm>

namespace ketplus::vscode_detail {
void report(ImportReport& out, ImportDisposition kind, const QString& source, const QString& target,
            const QString& reason) {
    if (out.items.size() < 4096)
        out.items.append({kind, source, target, reason});
    else if (out.items.size() == 4096)
        out.items.append(
            {ImportDisposition::Skipped,
             {},
             {},
             QStringLiteral("Report capped at 4,096 details; further entries omitted")});
}
void failure(ImportReport& out, const QString& source, const QString& message) {
    out.diagnostics.append({DiagnosticSeverity::Error, source, message});
}
bool failed(const ImportReport& out) {
    return std::any_of(out.diagnostics.cbegin(), out.diagnostics.cend(),
                       [](const auto& d) { return d.severity == DiagnosticSeverity::Error; });
}
std::optional<QColor> parseColor(const QJsonValue& value) {
    if (!value.isString())
        return {};
    auto hex = value.toString();
    if (hex.size() == 5 && hex.startsWith('#')) {
        QString expanded = QStringLiteral("#");
        for (qsizetype i = 1; i < hex.size(); ++i) {
            expanded += hex[i];
            expanded += hex[i];
        }
        hex = expanded;
    }
    return ColorSchemeReader::parseColor(hex);
}
void mapWorkbench(const AuthoredColors& colors, ColorScheme& scheme, ImportReport& out) {
    QSet<QString> known, used;
    for (const auto& mapping : workbenchMappings) {
        for (const auto* id : mapping.fallback)
            if (id)
                known.insert(QLatin1String(id));
        for (size_t i = 0; i < mapping.fallback.size(); ++i) {
            const auto* id = mapping.fallback[i];
            if (!id)
                continue;
            const auto found = colors.constFind(QLatin1String(id));
            if (found == colors.cend())
                continue;
            (scheme.*mapping.group).insert(QLatin1String(mapping.key), found->value);
            used.insert(QLatin1String(id));
            report(out, i == 0 ? ImportDisposition::Mapped : ImportDisposition::Approximated,
                   found->source, QLatin1String(mapping.target),
                   i == 0
                       ? QStringLiteral("Explicit workbench token mapping")
                       : QStringLiteral(
                             "First available fallback color used for nearest KetPlus component"));
            break;
        }
    }
    for (auto it = colors.cbegin(); it != colors.cend(); ++it)
        if (!used.contains(it.key()))
            report(
                out, ImportDisposition::Skipped, it->source, {},
                known.contains(it.key())
                    ? QStringLiteral("Higher-priority fallback supplied the target")
                    : QStringLiteral("Workbench color ID has no corresponding KetPlus component"));
}
} // namespace ketplus::vscode_detail
