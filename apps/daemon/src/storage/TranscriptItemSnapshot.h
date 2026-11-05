#pragma once

#include <QJsonObject>

namespace ketplus::daemon {

inline bool hasAuthoritativeAssistantText(const QJsonObject& payload) {
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    if (!item.isEmpty()) {
        return item.value(QStringLiteral("type")) == QStringLiteral("agentMessage") &&
               item.value(QStringLiteral("text")).isString();
    }
    return payload.value(QStringLiteral("item_type")) == QStringLiteral("agentMessage") &&
           payload.value(QStringLiteral("text")).isString();
}

inline bool isAssistantItem(const QJsonObject& payload) {
    const QJsonObject item = payload.value(QStringLiteral("item")).toObject();
    return item.isEmpty()
               ? payload.value(QStringLiteral("item_type")) == QStringLiteral("agentMessage")
               : item.value(QStringLiteral("type")) == QStringLiteral("agentMessage");
}

} // namespace ketplus::daemon
