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
    packets.append(encodeTransfer(transferId, bytes));
    return packets;
}

QList<Packet> FrameCodec::encodeTransfer(quint64 transferId, const QByteArray& bytes) {
    if (transferId == 0 || bytes.isEmpty() || bytes.size() > transferLimit)
        return {};
    QList<Packet> packets;
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

bool FrameCodec::admit(const Packet& packet, qint64 now, QString* error) {
    error->clear();
    if (packet.bytes.isEmpty() || packet.bytes.size() > messageLimit || expire(now)) {
        *error = QStringLiteral("frame_limit");
        return false;
    }
    return true;
}

std::optional<FrameCodec::CompletedTransfer> FrameCodec::assemble(const Packet& packet, qint64 now,
                                                                  QString* error) {
    if (packet.channel != Channel::Bulk || packet.bytes.size() <= 12) {
        *error = QStringLiteral("invalid_transfer");
        return std::nullopt;
    }
    const auto id = qFromBigEndian<quint64>(packet.bytes.constData());
    const auto index = qFromBigEndian<quint32>(packet.bytes.constData() + 8);
    if (!id || (!transfers_.contains(id) &&
                ((index & 0x7fffffffU) != 0 || transfers_.size() >= transferCountLimit))) {
        *error = QStringLiteral("transfer_limit");
        return std::nullopt;
    }
    auto& transfer = transfers_[id];
    if ((index & 0x7fffffffU) != transfer.next ||
        transfer.bytes.size() + packet.bytes.size() - 12 > transferLimit) {
        transfers_.remove(id);
        *error = QStringLiteral("invalid_transfer_sequence");
        return std::nullopt;
    }
    if (transfer.next++ == 0)
        transfer.started = now;
    transfer.bytes.append(packet.bytes.constData() + 12, packet.bytes.size() - 12);
    if ((index & 0x80000000U) == 0)
        return std::nullopt;
    CompletedTransfer done{id, transfer.bytes};
    transfers_.remove(id);
    return done;
}

QJsonObject FrameCodec::receive(const Packet& packet, qint64 now, QString* error) {
    if (!admit(packet, now, error))
        return {};
    if (!packet.binary)
        return decode(packet.bytes, error);
    const auto done = assemble(packet, now, error);
    return done ? decode(done->bytes, error) : QJsonObject{};
}

std::optional<FrameCodec::CompletedTransfer>
FrameCodec::receiveTransfer(const Packet& packet, qint64 now, QString* error) {
    if (!admit(packet, now, error))
        return std::nullopt;
    if (!packet.binary) {
        *error = QStringLiteral("invalid_transfer");
        return std::nullopt;
    }
    return assemble(packet, now, error);
}

bool FrameCodec::expire(qint64 now) {
    return transfers_.removeIf([now, timeout = transferTimeoutMs_](const auto& entry) {
        return now - entry.value().started > timeout;
    }) != 0;
}
} // namespace ketplus::daemon::transport
