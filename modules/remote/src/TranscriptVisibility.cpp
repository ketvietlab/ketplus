#include "TranscriptVisibility.h"

namespace ketplus::remote::transcript_visibility {
namespace {
constexpr auto kServer = "ketplus_agents";

QString itemType(const QJsonObject& payload, const QJsonObject& item) {
    return item.value(QStringLiteral("type"))
        .toString(payload.value(QStringLiteral("item_type")).toString());
}
} // namespace

bool internalTool(const QJsonObject& item) {
    const QString server = item.value(QStringLiteral("server")).toString();
    // Cursor names the server in the call's ACP input, beside a display title.
    if (server == QLatin1String(kServer) || item.value(QStringLiteral("arguments"))
                                                    .toObject()
                                                    .value(QStringLiteral("providerIdentifier"))
                                                    .toString() == QLatin1String(kServer))
        return true;
    const QString tool =
        item.value(QStringLiteral("tool")).toString(item.value(QStringLiteral("name")).toString());
    return server.isEmpty() &&
           tool.startsWith(QStringLiteral("mcp__%1__").arg(QLatin1String(kServer)));
}

bool hidden(const QString& eventType, const QJsonObject& payload) {
    if (eventType == QStringLiteral("tasks.changed"))
        return true;
    if (eventType != QStringLiteral("item.started") &&
        eventType != QStringLiteral("item.completed") &&
        eventType != QStringLiteral("item.updated"))
        return false;
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    const QString type = itemType(payload, item);
    if (type != QStringLiteral("mcpToolCall") && type != QStringLiteral("dynamicToolCall"))
        return false;
    // An item journaled flat keeps its tool beside the payload's other fields.
    return internalTool(item.isEmpty() ? payload : item);
}

} // namespace ketplus::remote::transcript_visibility
