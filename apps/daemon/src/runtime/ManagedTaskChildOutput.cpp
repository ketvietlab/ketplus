#include "runtime/ManagedTaskBridge.h"

#include <QCryptographicHash>

namespace ketplus::daemon {
void ManagedTaskBridge::recordChildEvent(const QJsonObject& event) {
    if (!runtime_)
        return;
    const auto type = event.value("event_type").toString();
    if (type != "assistant.delta" && type != "item.completed" && type != "user.message")
        return;
    const auto session = event.value("session_id").toString();
    const auto generation = event.value("session_generation").toInteger();
    const auto linked = ledger_.linkedTask(session, generation).value("task").toObject();
    if (linked.isEmpty())
        return;
    const auto id = linked.value("task_id").toString();
    if (type == "user.message") {
        streamedItems_.remove(id);
        return;
    }
    const auto payload = event.value("payload").toObject();
    const auto item = payload.value("item").toObject();
    const auto itemId = payload.value("item_id").toString(item.value("id").toString());
    const auto digest = QCryptographicHash::hash(itemId.toUtf8(), QCryptographicHash::Sha256);
    auto& streamed = streamedItems_[id];
    QString text;
    if (type == "assistant.delta") {
        text = payload.value("delta").toString();
        if (!streamed.contains(digest)) {
            if (streamed.size() >= 512)
                streamed.removeFirst();
            streamed.append(digest);
        }
    } else if (item.value("type") == "agentMessage" && !streamed.contains(digest)) {
        text = item.value("text").toString();
    }
    if (!text.isEmpty())
        childOutput(session, generation, event.value("cursor").toInteger(), text);
}
} // namespace ketplus::daemon
