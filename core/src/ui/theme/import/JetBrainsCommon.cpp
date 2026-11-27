#include "JetBrainsImport_p.h"
#include <QCryptographicHash>
#include <QFileInfo>

namespace ketplus::jetbrains_detail {
void item(ImportReport& report, ImportDisposition disposition, const QString& source,
          const QString& target, const QString& reason) {
    if (report.items.size() > 4096)
        return;
    if (report.items.size() == 4096) {
        report.items.append(
            {ImportDisposition::Skipped,
             {},
             {},
             QStringLiteral("Report limited to 4,096 items; additional details omitted")});
        return;
    }
    report.items.append({disposition, source, target, reason});
}
void error(ImportReport& report, const QString& source, const QString& message) {
    report.diagnostics.append({DiagnosticSeverity::Error, source, message});
}
bool hasErrors(const ImportReport& report) {
    for (const auto& d : report.diagnostics)
        if (d.severity == DiagnosticSeverity::Error)
            return true;
    return false;
}
std::optional<QColor> color(QString value, bool xml) {
    if (value.startsWith('#'))
        value.remove(0, 1);
    for (const auto c : value) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F')))
            return {};
    }
    if (xml && !value.isEmpty() && value.size() < 6)
        value = value.rightJustified(6, u'0');
    if (value.size() != 6 && value.size() != 8)
        return {};
    return ColorSchemeReader::parseColor('#' + value);
}
void setColor(ColorScheme& scheme, const QString& target, const QColor& value) {
    const auto dot = target.indexOf('.');
    const auto group = target.left(dot), key = target.mid(dot + 1);
    if (group == u"ui")
        scheme.ui[key] = value;
    else if (group == u"editor")
        scheme.editor[key] = value;
    else if (group == u"terminal")
        scheme.terminal[key] = value;
    else if (group == u"diff")
        scheme.diff[key] = value;
}
ColorScheme newScheme(const QString& path, const QByteArray& bytes, const QString& name) {
    ColorScheme scheme;
    scheme.id = QStringLiteral("jetbrains-") +
                QString::fromLatin1(QCryptographicHash::hash(path.toUtf8() + '\0' + bytes,
                                                             QCryptographicHash::Sha256)
                                        .toHex()
                                        .left(32));
    scheme.name = name.trimmed().isEmpty() ? QFileInfo(path).completeBaseName() : name.trimmed();
    scheme.metadata = {{QStringLiteral("sourceApp"), QStringLiteral("JetBrains")},
                       {QStringLiteral("sourceFile"), path}};
    return scheme;
}
void setKind(ColorScheme& scheme, bool dark) {
    scheme.kind = dark ? SchemeKind::Dark : SchemeKind::Light;
    scheme.extends = dark ? QStringLiteral("builtin-dark") : QStringLiteral("builtin-light");
}
bool Source::read(const QString& path, QByteArray& data, ImportReport& report) {
    ket::theme::io::Error ioError;
    if (!resolver.read(path, &data, &ioError)) {
        error(report, path, ioError.message);
        return false;
    }
    bytesRead += quint64(data.size());
    if (bytesRead > ket::theme::io::MaxExpandedBytes) {
        data.clear();
        error(report, path, QStringLiteral("Import exceeds 100 MiB cumulative read budget"));
        return false;
    }
    return true;
}
} // namespace ketplus::jetbrains_detail
