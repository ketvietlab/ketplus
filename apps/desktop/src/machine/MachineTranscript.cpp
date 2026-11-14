#include "machine/MachineTranscript.h"

#include "MachineRows.h"

#include <utility>

namespace ketplus {

namespace {

qint64 cursorOf(const QJsonValue& event) {
    return event.toObject().value(QLatin1String("cursor")).toInteger();
}

} // namespace

void MachineTranscript::reset(const QJsonArray& events) {
    events_ = {};
    for (const QJsonValue& event : events)
        events_.append(remote::windowEvent(event.toObject()));
    rebuild();
}

void MachineTranscript::prepend(const QJsonArray& older) {
    const qint64 first = firstCursor();
    QJsonArray merged;
    for (const QJsonValue& value : older) {
        const qint64 cursor = cursorOf(value);
        if (cursor > 0 && (first == 0 || cursor < first) &&
            (merged.isEmpty() || cursor > cursorOf(merged.last())))
            merged.append(remote::windowEvent(value.toObject()));
    }
    if (merged.isEmpty())
        return;
    for (const QJsonValue& value : std::as_const(events_))
        merged.append(value);
    events_ = merged;
    rebuild();
}

qint64 MachineTranscript::firstCursor() const {
    for (const QJsonValue& value : events_)
        if (const qint64 cursor = cursorOf(value); cursor > 0)
            return cursor;
    return 0;
}

void MachineTranscript::rebuild() {
    items_.clear();
    lastCursor_ = 0;
    const QJsonArray events = std::exchange(events_, {});
    for (const QJsonValue& event : events)
        apply(event.toObject());
}

bool MachineTranscript::apply(const QJsonObject& delivered) {
    // A window's event, or a live session.event (payload {event_type, data}).
    const QJsonObject event = remote::windowEvent(delivered);
    const qint64 cursor = event.value(QLatin1String("cursor")).toInteger();
    if (cursor > 0) {
        if (cursor <= lastCursor_)
            return false;
        lastCursor_ = cursor;
    }
    events_.append(event);
    return applyRow(event);
}

bool MachineTranscript::applyRow(const QJsonObject& event) {
    const QString type = event.value(QLatin1String("event_type")).toString();
    const QJsonObject payload = event.value(QLatin1String("payload")).toObject();
    if (type == QLatin1String("user.message") || type == QLatin1String("turn.steered")) {
        const QString text = payload.value(QLatin1String("text")).toString();
        if (text.isEmpty())
            return false;
        items_.append({Kind::User, text, {}});
        return true;
    }
    if (type == QLatin1String("assistant.delta")) {
        const QString itemId = payload.value(QLatin1String("item_id")).toString();
        const QString delta = payload.value(QLatin1String("delta")).toString();
        if (!items_.isEmpty() && items_.last().kind == Kind::Assistant &&
            items_.last().itemId == itemId) {
            items_.last().text += delta;
        } else {
            items_.append({Kind::Assistant, delta, itemId});
        }
        return true;
    }
    if (type == QLatin1String("item.completed")) {
        const QJsonObject item = payload.value(QLatin1String("item")).toObject();
        const QString itemType = item.value(QLatin1String("type")).toString();
        const QString itemId = item.value(QLatin1String("id")).toString();
        if (itemType == QLatin1String("agentMessage")) {
            const QString text = item.value(QLatin1String("text")).toString();
            // The completed message replaces the streamed deltas of the same item.
            for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
                if (it->kind == Kind::Assistant && it->itemId == itemId && !itemId.isEmpty()) {
                    it->text = text;
                    return true;
                }
            }
            items_.append({Kind::Assistant, text, itemId});
            return true;
        }
        if (itemType == QLatin1String("commandExecution")) {
            items_.append(
                {Kind::Activity, item.value(QLatin1String("command")).toString(), itemId});
            return true;
        }
        return false;
    }
    if (type == QLatin1String("error")) {
        const QString message = payload.value(QLatin1String("message")).toString();
        items_.append({Kind::Error, message.isEmpty() ? QStringLiteral("Error") : message, {}});
        return true;
    }
    return false;
}

} // namespace ketplus
