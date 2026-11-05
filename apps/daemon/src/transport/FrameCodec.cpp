#include "transport/FrameCodec.h"

#include <QJsonDocument>
#include <QtEndian>

namespace ketplus::daemon::transport {
namespace {
QJsonObject decode(const QByteArray& bytes, QString* error) {
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject() ||
        doc.object().value("type").toString().isEmpty()) {
        *error = QStringLiteral("invalid_frame");
        return {};
    }
    return doc.object();
}
} // namespace

QList<Packet> FrameCodec::encode(const QJsonObject& frame, Channel channel, quint64 transferId) {
    const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (bytes.size() <= messageLimit)
        return {{channel, bytes, false}};
    if (bytes.size() > transferLimit || transferId == 0)
        return {};
    QList<Packet> packets;
    if (frame.value("type") == "event") {
        const auto event = frame.value("event").toObject();
        const QJsonObject marker{{"type", "event.transfer"},
                                 {"epoch", frame.value("epoch")},
                                 {"sessionId", event.value("sessionId")},
                                 {"cursor", event.value("cursor")},
                                 {"transferId", QString::number(transferId)}};
        packets.append(
            {Channel::Events, QJsonDocument(marker).toJson(QJsonDocument::Compact), false});
    }
    constexpr qsizetype partSize = messageLimit - 12;
    quint32 index = 0;
    for (qsizetype offset = 0; offset < bytes.size(); offset += partSize, ++index) {
        const auto part = bytes.mid(offset, partSize);
        QByteArray packet(12, '\0');
        qToBigEndian(transferId, packet.data());
        qToBigEndian(index | (offset + part.size() == bytes.size() ? 0x80000000U : 0U),
                     packet.data() + 8);
        packet.append(part);
        packets.append({Channel::Bulk, packet, true});
    }
    return packets;
}

QJsonObject FrameCodec::receive(const Packet& packet, qint64 now, QString* error) {
    error->clear();
    if (packet.bytes.isEmpty() || packet.bytes.size() > messageLimit || expire(now)) {
        *error = QStringLiteral("frame_limit");
        return {};
    }
    if (!packet.binary)
        return decode(packet.bytes, error);
    if (packet.channel != Channel::Bulk || packet.bytes.size() <= 12) {
        *error = QStringLiteral("invalid_transfer");
        return {};
    }
    const auto id = qFromBigEndian<quint64>(packet.bytes.constData());
    const auto index = qFromBigEndian<quint32>(packet.bytes.constData() + 8);
    if (!id || (!transfers_.contains(id) &&
                ((index & 0x7fffffffU) != 0 || transfers_.size() >= transferCountLimit))) {
        *error = QStringLiteral("transfer_limit");
        return {};
    }
    auto& transfer = transfers_[id];
    if ((index & 0x7fffffffU) != transfer.next ||
        transfer.bytes.size() + packet.bytes.size() - 12 > transferLimit) {
        transfers_.remove(id);
        *error = QStringLiteral("invalid_transfer_sequence");
        return {};
    }
    if (transfer.next++ == 0)
        transfer.started = now;
    transfer.bytes.append(packet.bytes.constData() + 12, packet.bytes.size() - 12);
    if ((index & 0x80000000U) == 0)
        return {};
    const auto bytes = transfer.bytes;
    transfers_.remove(id);
    return decode(bytes, error);
}

bool FrameCodec::expire(qint64 now) {
    return transfers_.removeIf(
               [now](const auto& entry) { return now - entry.value().started > 30'000; }) != 0;
}
} // namespace ketplus::daemon::transport
