#include "VsCodeImport_p.h"
#include <functional>

namespace ketplus::vscode_detail {
bool plistRules(const QByteArray& bytes, const QString& path, QList<Rule>& rules,
                ImportReport& out) {
    ket::theme::io::XmlNode root;
    ket::theme::io::Error error;
    if (!ket::theme::io::parseXml(bytes, &root, &error)) {
        failure(out, path,
                QStringLiteral("%1 (line %2, column %3)")
                    .arg(error.message)
                    .arg(error.line)
                    .arg(error.column));
        return false;
    }
    if (root.name != u"plist" || root.children.size() != 1) {
        failure(out, path, QStringLiteral("Expected plist with one dictionary"));
        return false;
    }
    bool valid = true;
    std::function<QJsonValue(const ket::theme::io::XmlNode&, int)> decode =
        [&](const auto& node, int depth) -> QJsonValue {
        if (depth > 64) {
            valid = false;
            return {};
        }
        if (node.name == u"string")
            return node.text;
        if (node.name == u"true")
            return true;
        if (node.name == u"false")
            return false;
        if (node.name == u"dict") {
            QJsonObject object;
            if (node.children.size() % 2) {
                valid = false;
                return {};
            }
            for (qsizetype i = 0; i < node.children.size(); i += 2) {
                const auto& key = node.children[i];
                if (key.name != u"key" || object.contains(key.text)) {
                    valid = false;
                    return {};
                }
                object.insert(key.text, decode(node.children[i + 1], depth + 1));
            }
            return object;
        }
        if (node.name == u"array") {
            QJsonArray array;
            for (const auto& child : node.children)
                array.append(decode(child, depth + 1));
            return array;
        }
        // Numeric/date/data metadata is inert and irrelevant to token settings.
        report(out, ImportDisposition::Skipped, path + '/' + node.name, {},
               QStringLiteral("Unsupported plist value type omitted"));
        return {};
    };
    const auto decoded = decode(root.children.first(), 0);
    if (!valid || !decoded.isObject() ||
        !decoded.toObject().value(QStringLiteral("settings")).isArray()) {
        failure(out, path, QStringLiteral("Malformed tmTheme dictionary/settings array"));
        return false;
    }
    const auto settings = decoded.toObject().value(QStringLiteral("settings")).toArray();
    for (qsizetype i = 0; i < settings.size(); ++i) {
        if (!settings[i].isObject()) {
            failure(out, path, QStringLiteral("tmTheme rule must be a dictionary"));
            return false;
        }
        rules.append(
            {settings[i].toObject(), path + QStringLiteral("/settings/") + QString::number(i)});
    }
    report(out, ImportDisposition::Approximated, path, QStringLiteral("syntax"),
           QStringLiteral(
               "tmTheme plist decoded without DTD resolution; shared-role selector limits apply"));
    return true;
}
} // namespace ketplus::vscode_detail
