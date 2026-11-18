#include "protocol/FrameRules.h"
#include "runner/link/LinkLog.h"
#include "runner/link/RunnerLinkState.h"
#include "runner/link/RunnerLinks.h"

namespace ketplus::daemon::runner::link {
namespace rules = ketplus::runner::protocol::rules;
using transport::FrameCodec;

void RunnerLinks::send(const QString& connectionId, const FrameChannel channel,
                       const QByteArray& message) {
    Link* link = find(connectionId);
    if (!link || link->state != Link::State::Open) {
        qCInfo(lcRunnerLink) << "runner frame not sent: the connection is not open";
        return;
    }
    if (channel == FrameChannel::Bulk || message.isEmpty() ||
        message.size() > FrameCodec::messageLimit) {
        qCWarning(lcRunnerLink) << "runner frame not sent:" << message.size() << "bytes";
        return;
    }
    const auto keep = m_links.value(connectionId);
    if (!link->transport->send(channel == FrameChannel::Control ? TransportChannel::Control
                                                                : TransportChannel::Events,
                               message, false) &&
        !link->dropped)
        fail(connectionId, QStringLiteral("send_failed"));
}

void RunnerLinks::sendBulk(const QString& connectionId, const QString& transferId,
                           const QByteArray& bytes) {
    Link* link = find(connectionId);
    if (!link || link->state != Link::State::Open) {
        qCInfo(lcRunnerLink) << "runner transfer not sent: the connection is not open";
        return;
    }
    bool ok = false;
    const quint64 id = rules::isLowerHex(transferId, 16) ? transferId.toULongLong(&ok, 16) : 0;
    const QList<transport::Packet> packets =
        ok && !bytes.isEmpty() && bytes.size() <= transferSizeLimit
            ? FrameCodec::encodeTransfer(id, bytes)
            : QList<transport::Packet>{};
    if (packets.isEmpty()) {
        qCWarning(lcRunnerLink) << "runner transfer not sent:" << bytes.size() << "bytes";
        return;
    }
    // A caller that ignores bulkWritable() must not grow the buffer without bound, and a part
    // dropped in silence would leave the transfer hanging: the connection ends instead.
    if (link->transport->buffered(TransportChannel::Bulk) + quint64(bytes.size()) >
        bulkBufferLimit) {
        fail(connectionId, QStringLiteral("bulk_overflow"));
        return;
    }
    const auto keep = m_links.value(connectionId);
    // A part that could not go leaves the transfer broken: the connection ends.
    for (const auto& packet : packets) {
        if (link->dropped)
            return;
        if (!link->transport->send(packet.channel, packet.bytes, packet.binary)) {
            if (!link->dropped)
                fail(connectionId, QStringLiteral("send_failed"));
            return;
        }
    }
}

bool RunnerLinks::bulkWritable(const QString& connectionId) const {
    Link* link = find(connectionId);
    if (!link || link->state != Link::State::Open)
        return false;
    const bool writable = link->transport->buffered(TransportChannel::Bulk) < bulkWritableLimit;
    if (!writable)
        link->saidNotWritable = true;
    return writable;
}

void RunnerLinks::writable(Link& link) {
    if (link.state != Link::State::Open || !link.saidNotWritable ||
        link.transport->buffered(TransportChannel::Bulk) >= bulkWritableLimit)
        return;
    link.saidNotWritable = false;
    m_events.bulkWritable(link.offer.connectionId);
}

void RunnerLinks::received(Link& link, const TransportChannel channel, const QByteArray& bytes,
                           const bool binary) {
    if (link.state != Link::State::Open)
        return;
    const QString id = link.offer.connectionId;
    // The runner sends no transfers: anything on `bulk` breaks the channel rules.
    if (channel == TransportChannel::Bulk || binary) {
        fail(id, QStringLiteral("invalid_transfer"));
        return;
    }
    if (bytes.isEmpty() || bytes.size() > FrameCodec::messageLimit) {
        fail(id, QStringLiteral("frame_limit"));
        return;
    }
    m_events.frameReceived(
        id, channel == TransportChannel::Control ? FrameChannel::Control : FrameChannel::Events,
        bytes);
}

} // namespace ketplus::daemon::runner::link
