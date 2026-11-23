#include "control/RelayFrames.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

namespace ketplus::remote::frames {
namespace {
void fail(QString* error, const QString& reason) {
    if (error)
        *error = reason;
}

bool validSessionId(const QString& id) {
    return !id.isEmpty() && id.toUtf8().size() <= sessionIdLimit;
}

// An integer JSON number that is not fractional and that a double carries exactly (up to
// 2^53, the range a JSON number keeps for every client); anything else, including values
// no 64-bit integer holds, is refused before it is converted.
std::optional<qint64> integer(const QJsonValue& value) {
    constexpr double exactLimit = 9007199254740992.0; // 2^53
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < -exactLimit || number > exactLimit ||
        number != std::trunc(number))
        return std::nullopt;
    return static_cast<qint64>(number);
}

QByteArray seal(const QJsonObject& frame, QString* error) {
    const QByteArray bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (bytes.size() > outboundLimit) {
        fail(error, QStringLiteral("The frame is larger than %1 bytes.").arg(outboundLimit));
        return {};
    }
    return bytes;
}

// Reads the events of a page or the one of an event frame; every cursor is a positive integer
// above the one before it.
bool readEvents(const QJsonArray& events, Frame* frame, QString* error) {
    if (events.size() > pageEventLimit) {
        fail(error, QStringLiteral("A page carries more than %1 events.").arg(pageEventLimit));
        return false;
    }
    qint64 previous = 0;
    for (const QJsonValue& value : events) {
        if (!value.isObject()) {
            fail(error, QStringLiteral("An event is not an object."));
            return false;
        }
        const auto event = value.toObject();
        const auto cursor = integer(event.value(QStringLiteral("cursor")));
        if (!cursor || *cursor <= previous) {
            fail(error, QStringLiteral("Event cursors are missing or not increasing."));
            return false;
        }
        previous = *cursor;
        // A generation that is present must be a valid number: ignoring one would take the
        // event for the followed generation.
        if (const auto field = event.value(QStringLiteral("session_generation"));
            !field.isUndefined()) {
            const auto generation = integer(field);
            if (!generation || *generation < 1 ||
                (frame->generation != 0 && frame->generation != *generation)) {
                fail(error, QStringLiteral("An event generation is invalid or differs."));
                return false;
            }
            frame->generation = *generation;
        }
    }
    frame->events = events;
    return true;
}
} // namespace

QByteArray encodeSubscribe(const QString& sessionId, const qint64 generation,
                           const qint64 afterCursor, QString* error) {
    if (!validSessionId(sessionId) || generation < 1 || afterCursor < 0) {
        fail(error, QStringLiteral("A subscription needs a conversation, generation and cursor."));
        return {};
    }
    return seal({{QStringLiteral("type"), QStringLiteral("subscribe")},
                 {QStringLiteral("sessionId"), sessionId},
                 {QStringLiteral("sessionGeneration"), generation},
                 {QStringLiteral("afterCursor"), afterCursor}},
                error);
}

QByteArray encodeUnsubscribe(const QString& sessionId, QString* error) {
    if (!validSessionId(sessionId)) {
        fail(error, QStringLiteral("An unsubscribe needs a conversation."));
        return {};
    }
    return seal({{QStringLiteral("type"), QStringLiteral("unsubscribe")},
                 {QStringLiteral("sessionId"), sessionId}},
                error);
}

std::optional<Frame> decode(const QByteArray& bytes, QString* error) {
    if (bytes.size() > inboundLimit) {
        fail(error, QStringLiteral("The frame is larger than %1 bytes.").arg(inboundLimit));
        return std::nullopt;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, QStringLiteral("The frame is not a JSON object."));
        return std::nullopt;
    }
    const auto object = document.object();
    const QString name = object.value(QStringLiteral("type")).toString();
    Frame frame;
    frame.sessionId = object.value(QStringLiteral("sessionId")).toString();
    if (!frame.sessionId.isEmpty() && !validSessionId(frame.sessionId)) {
        fail(error, QStringLiteral("The conversation id is too long."));
        return std::nullopt;
    }
    if (const auto field = object.value(QStringLiteral("sessionGeneration"));
        !field.isUndefined()) {
        const auto generation = integer(field);
        if (!generation || *generation < 0) {
            fail(error, QStringLiteral("The generation is invalid."));
            return std::nullopt;
        }
        frame.generation = *generation;
    }

    if (name == QLatin1String("frames.dropped")) {
        frame.type = Type::FramesDropped;
        return frame;
    }
    if (frame.sessionId.isEmpty()) {
        fail(error, QStringLiteral("The frame names no conversation."));
        return std::nullopt;
    }
    if (name == QLatin1String("gap")) {
        const auto after = integer(object.value(QStringLiteral("afterCursor")));
        if (!after || *after < 0) {
            fail(error, QStringLiteral("A gap needs a cursor."));
            return std::nullopt;
        }
        frame.type = Type::Gap;
        frame.afterCursor = *after;
        return frame;
    }
    if (name == QLatin1String("session.reset")) {
        if (frame.generation < 1) {
            fail(error, QStringLiteral("A reset needs a generation."));
            return std::nullopt;
        }
        frame.type = Type::SessionReset;
        return frame;
    }
    if (name == QLatin1String("events")) {
        const auto events = object.value(QStringLiteral("events"));
        if (!events.isArray() || !readEvents(events.toArray(), &frame, error)) {
            if (!events.isArray())
                fail(error, QStringLiteral("A page carries no events array."));
            return std::nullopt;
        }
        frame.type = Type::Events;
        frame.live = object.value(QStringLiteral("live")).toBool(false);
        return frame;
    }
    if (name == QLatin1String("event")) {
        const auto event = object.value(QStringLiteral("event"));
        if (!event.isObject() || !readEvents(QJsonArray{event}, &frame, error)) {
            if (!event.isObject())
                fail(error, QStringLiteral("An event frame carries no event."));
            return std::nullopt;
        }
        frame.type = Type::Event;
        return frame;
    }
    fail(error, QStringLiteral("The frame type is not known."));
    return std::nullopt;
}

} // namespace ketplus::remote::frames
