#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <stdexcept>

namespace r03 {

struct NormalizedEvent {
    QString type;
    QJsonObject payload;
};

// Test-only mapping to SessionHost::handleProviderNotification's current payloads.
// Workload cursor/turn fields are references; LocalJournal assigns its own values.
inline NormalizedEvent adapt(const QJsonObject& event) {
    const QString type = event.value("event_type").toString();
    const QJsonObject payload = event.value("payload").toObject();
    if (type == "assistant.delta") {
        if (!payload.value("text_delta").isString() || !payload.value("item_id").isString())
            throw std::runtime_error("unsupported assistant delta shape");
        return {type,
                {{"item_id", payload.value("item_id")}, {"delta", payload.value("text_delta")}}};
    }
    if (type == "command.output_delta") {
        if (!payload.value("text").isString() || !payload.value("item_id").isString())
            throw std::runtime_error("unsupported command delta shape");
        return {type, {{"item_id", payload.value("item_id")}, {"delta", payload.value("text")}}};
    }
    if (type != "item.started" && type != "item.completed")
        throw std::runtime_error(
            "unsupported event type (aggregate requires explicit lifecycle folding)");
    QJsonObject item = payload.value("item").toObject();
    if (!item.value("id").isString())
        throw std::runtime_error("unsupported item identity");
    const QString itemType = item.value("type").toString();
    if (itemType == "message") {
        if (item.value("role") == "user" && type == "item.completed" &&
            item.value("text").isString())
            return {"user.message", {{"text", item.value("text")}}};
        if (item.value("role") != "assistant")
            throw std::runtime_error("unsupported message role");
        item.remove("role");
        item.insert("type", "agentMessage");
    } else if (itemType == "command") {
        if (!item.value("command").isString() || !item.value("status").isString())
            throw std::runtime_error("unsupported command lifecycle");
        item.insert("type", "commandExecution");
    } else {
        throw std::runtime_error("unsupported workload item type; tool/media mapping not inferred");
    }
    QJsonObject normalized{
        {"item_id", item.value("id")}, {"item_type", item.value("type")}, {"item", item}};
    for (const QString& key : {QString("text"), QString("command"), QString("status")})
        if (item.contains(key))
            normalized.insert(key, item.value(key));
    return {type, normalized};
}

// The workload aggregate record is an operation on a command's final provider item,
// not a production event. Only stdout + empty stderr is supported by this mapping.
inline QList<NormalizedEvent> adaptCommandScenario(const QJsonArray& events) {
    QList<NormalizedEvent> result;
    QJsonObject pending;
    for (const auto& value : events) {
        const auto event = value.toObject();
        const auto payload = event.value("payload").toObject();
        if (event.value("event_type") == "command.output_aggregated") {
            if (!pending.isEmpty() || !payload.value("stdout").isString() ||
                payload.value("stderr") != "")
                throw std::runtime_error("unsupported aggregate shape/order");
            pending = payload;
            continue;
        }
        auto mapped = adapt(event);
        if (!pending.isEmpty()) {
            if (mapped.type != "item.completed" ||
                mapped.payload.value("item_id") != pending.value("item_id") ||
                mapped.payload.value("item_type") != "commandExecution")
                throw std::runtime_error("aggregate must precede matching command completion");
            auto item = mapped.payload.value("item").toObject();
            item.insert("aggregatedOutput", pending.value("stdout"));
            mapped.payload.insert("item", item);
            pending = {};
        }
        result.append(mapped);
    }
    if (!pending.isEmpty())
        throw std::runtime_error("unconsumed aggregate");
    return result;
}

} // namespace r03
