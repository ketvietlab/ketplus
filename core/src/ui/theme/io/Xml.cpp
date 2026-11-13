#include "ThemeInput.h"
#include <QXmlStreamReader>

namespace ket::theme::io {
bool parseXml(const QByteArray& data, XmlNode* root, Error* error) {
    if (error)
        *error = {};
    if (root)
        *root = {};
    if (!root || data.size() > MaxFileBytes) {
        if (error)
            error->message = QStringLiteral("XML requires an output and at most 5 MiB");
        return false;
    }
    QXmlStreamReader xml(data);
    QVector<XmlNode> stack;
    XmlNode result;
    int nodes = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            xml.raiseError(QStringLiteral("DTD and external entities are forbidden"));
        } else if (token == QXmlStreamReader::StartElement) {
            if (stack.size() >= 128 || ++nodes > 100000) {
                xml.raiseError(QStringLiteral("XML nesting or node limit exceeded"));
                continue;
            }
            XmlNode node;
            node.name = xml.name().toString();
            for (const auto& a : xml.attributes())
                node.attributes.insert(a.qualifiedName().toString(), a.value().toString());
            stack.append(std::move(node));
        } else if (token == QXmlStreamReader::Characters && !stack.isEmpty()) {
            stack.last().text += xml.text();
        } else if (token == QXmlStreamReader::EndElement && !stack.isEmpty()) {
            auto node = stack.takeLast();
            if (stack.isEmpty())
                result = std::move(node);
            else
                stack.last().children.append(std::move(node));
        }
    }
    if (xml.hasError() || result.name.isEmpty()) {
        if (error)
            *error = {xml.hasError() ? xml.errorString() : QStringLiteral("XML has no root"),
                      {},
                      xml.lineNumber(),
                      xml.columnNumber()};
        return false;
    }
    *root = std::move(result);
    return true;
}
} // namespace ket::theme::io
