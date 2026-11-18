#include "peer/PeerChannels.h"

#include "peer/PeerLog.h"

namespace ketplus::runner::peer {
namespace {

using daemon::transport::FrameCodec;

std::optional<Channel> transportChannel(const protocol::Channel channel) {
    switch (channel) {
    case protocol::Channel::Control:
        return Channel::Control;
    case protocol::Channel::Events:
        return Channel::Events;
    case protocol::Channel::Bulk:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

PeerChannels::PeerChannels(PeerClock& clock, PeerChannelListener& listener)
    : m_clock(clock), m_listener(listener) {}

void PeerChannels::send(const protocol::Channel channel, const QByteArray& message) {
    const auto target = transportChannel(channel);
    if (!target || message.isEmpty() || message.size() > FrameCodec::messageLimit) {
        qCWarning(lcRunnerPeer) << "peer frame not sent: wrong channel or size" << message.size();
        return;
    }
    if (PeerTransport* transport = m_connector->openTransport())
        transport->send(*target, message, false);
}

bool PeerChannels::eventsWritable() const {
    const PeerTransport* transport = m_connector->openTransport();
    const bool writable = transport && transport->buffered(Channel::Events) < eventsBufferLimit;
    if (!writable)
        m_saidNotWritable = true;
    return writable;
}

void PeerChannels::close(const QString& reason) {
    m_codec.clear();
    m_saidNotWritable = false;
    m_connector->close(reason);
}

void PeerChannels::requestConnection() {
    m_codec.clear();
    m_saidNotWritable = false;
    if (m_request)
        m_connector->request(*m_request);
}

void PeerChannels::connectionOpened(const QString& connectionId) {
    m_codec.clear();
    m_saidNotWritable = false;
    m_listener.channelOpened(connectionId);
}

void PeerChannels::connectionLost(const QString& reason) {
    m_codec.clear();
    m_saidNotWritable = false;
    m_listener.channelLost(reason);
}

void PeerChannels::attemptFailed(const QString& reason) { m_listener.attemptFailed(reason); }

void PeerChannels::gaveUp(const QString& reason) { m_listener.gaveUp(reason); }

void PeerChannels::violation(const QString& reason) {
    qCWarning(lcRunnerPeer) << "peer channel violation:" << reason;
    m_connector->fail(reason);
}

void PeerChannels::received(const Channel channel, const QByteArray& bytes, const bool binary) {
    if (channel == Channel::Bulk) {
        if (!binary) {
            violation(QStringLiteral("invalid_transfer"));
            return;
        }
        QString error;
        const auto done = m_codec.receiveTransfer({channel, bytes, true},
                                                  m_clock.now().toMSecsSinceEpoch(), &error);
        if (!error.isEmpty())
            violation(error);
        else if (done)
            m_listener.transferReceived(done->transferId, done->bytes);
        return;
    }
    if (binary) {
        violation(QStringLiteral("invalid_transfer"));
        return;
    }
    if (bytes.isEmpty() || bytes.size() > FrameCodec::messageLimit) {
        violation(QStringLiteral("frame_limit"));
        return;
    }
    m_listener.frameReceived(channel == Channel::Control ? protocol::Channel::Control
                                                         : protocol::Channel::Events,
                             bytes);
}

void PeerChannels::writable() {
    if (m_saidNotWritable && eventsWritable()) {
        m_saidNotWritable = false;
        m_listener.eventsWritable();
    }
}

} // namespace ketplus::runner::peer
