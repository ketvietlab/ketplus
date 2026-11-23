#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <optional>

namespace ketplus::remote::frames {

// What a desktop sends: a subscribe or unsubscribe stays under this size.
constexpr qsizetype outboundLimit = 4 * 1024;
// What a desktop accepts in one received frame.
constexpr qsizetype inboundLimit = 64 * 1024;
constexpr int pageEventLimit = 100;
constexpr qsizetype sessionIdLimit = 256;

[[nodiscard]] QByteArray encodeSubscribe(const QString& sessionId, qint64 generation,
                                         qint64 afterCursor, QString* error = nullptr);
[[nodiscard]] QByteArray encodeUnsubscribe(const QString& sessionId, QString* error = nullptr);

enum class Type { Events, Event, Gap, SessionReset, FramesDropped };

struct Frame {
    Type type{Type::Events};
    // Empty for FramesDropped without a named conversation.
    QString sessionId;
    qint64 generation{0};
    // Gap: the cursor the host last delivered.
    qint64 afterCursor{0};
    // Events and Event: SessionEvent objects with strictly increasing cursors.
    QJsonArray events;
    // Events: the host has caught up and sends live events next.
    bool live{false};
};

// A received frame, or nothing and `error` for an oversize, malformed or unknown frame. The
// error names the reason and never carries event content.
[[nodiscard]] std::optional<Frame> decode(const QByteArray& bytes, QString* error = nullptr);

} // namespace ketplus::remote::frames
