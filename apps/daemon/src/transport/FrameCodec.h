#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace ketplus::daemon::transport {

enum class Channel { Control = 0, Events = 1, Bulk = 2 };
struct Packet {
    Channel channel{Channel::Control};
    QByteArray bytes;
    bool binary{false};
};

// One bounded JSON frame or an ordered binary transfer. Transfer IDs are decimal strings
// on the JSON side, avoiding loss of precision in JavaScript clients.
class FrameCodec final {
  public:
    static constexpr qsizetype messageLimit = 64 * 1024;
    static constexpr qsizetype transferLimit = 8 * 1024 * 1024;
    static constexpr int transferCountLimit = 4;
    static QList<Packet> encode(const QJsonObject& frame, Channel channel, quint64 transferId);
    QJsonObject receive(const Packet& packet, qint64 now, QString* error);
    bool expire(qint64 now);
    void clear() { transfers_.clear(); }

  private:
    struct Transfer {
        quint32 next{0};
        qint64 started{0};
        QByteArray bytes;
    };
    QHash<quint64, Transfer> transfers_;
};

} // namespace ketplus::daemon::transport
