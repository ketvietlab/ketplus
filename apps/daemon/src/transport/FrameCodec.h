#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

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
    static constexpr qint64 defaultTransferTimeoutMs = 30'000;

    FrameCodec() = default;
    // A transfer whose first part is older than `transferTimeoutMs` is dropped.
    explicit FrameCodec(qint64 transferTimeoutMs) : transferTimeoutMs_(transferTimeoutMs) {}

    static QList<Packet> encode(const QJsonObject& frame, Channel channel, quint64 transferId);
    QJsonObject receive(const Packet& packet, qint64 now, QString* error);

    // Raw bytes as one ordered binary transfer on `bulk`: empty when `transferId` is 0 or the
    // bytes are empty or above transferLimit.
    static QList<Packet> encodeTransfer(quint64 transferId, const QByteArray& bytes);
    struct CompletedTransfer {
        quint64 transferId{0};
        QByteArray bytes;
    };
    // The bytes of a transfer once its last part arrived, under the same limits as receive().
    // A text packet is an error here.
    std::optional<CompletedTransfer> receiveTransfer(const Packet& packet, qint64 now,
                                                     QString* error);

    bool expire(qint64 now);
    void clear() { transfers_.clear(); }

  private:
    bool admit(const Packet& packet, qint64 now, QString* error);
    std::optional<CompletedTransfer> assemble(const Packet& packet, qint64 now, QString* error);

    qint64 transferTimeoutMs_{defaultTransferTimeoutMs};
    struct Transfer {
        quint32 next{0};
        qint64 started{0};
        QByteArray bytes;
    };
    QHash<quint64, Transfer> transfers_;
};

} // namespace ketplus::daemon::transport
