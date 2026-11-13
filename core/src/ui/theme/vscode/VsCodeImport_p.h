#pragma once
#include "VsCodeImporter.h"
#include "ui/theme/io/ThemeInput.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>

namespace ketplus::vscode_detail {
void report(ImportReport& out, ImportDisposition kind, const QString& source, const QString& target,
            const QString& reason);
void failure(ImportReport& out, const QString& source, const QString& message);
bool failed(const ImportReport& out);
std::optional<QColor> parseColor(const QJsonValue& value);
struct Rule {
    QJsonObject value;
    QString source;
};
struct AuthoredColor {
    QColor value;
    QString source;
};
using AuthoredColors = QMap<QString, AuthoredColor>;
void mapWorkbench(const AuthoredColors& colors, ColorScheme& scheme, ImportReport& out);
void mapGlobalColors(const QList<Rule>& rules, AuthoredColors& colors,
                     const QSet<QString>& declared, ImportReport& out);
void mapRules(const QList<Rule>& rules, ColorScheme& scheme, ImportReport& out);
bool plistRules(const QByteArray& bytes, const QString& path, QList<Rule>& rules,
                ImportReport& out);
struct Session {
    const ket::theme::io::IncludeResolver& resolver;
    quint64 remaining = ket::theme::io::MaxExpandedBytes;
    bool read(const QString& path, QByteArray& bytes, ImportReport& out);
    bool convert(const QString& path, const QString& label, const QString& hint,
                 VsCodeImportedScheme& variant);
};
} // namespace ketplus::vscode_detail
