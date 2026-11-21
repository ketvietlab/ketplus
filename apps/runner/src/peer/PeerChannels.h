#pragma once

#include "peer/PeerConnector.h"
#include "session/SessionPorts.h"

#include <QByteArray>
#include <QString>

#include <optional>

// The session's PeerChannel over the connector's three channels: frames on `control` and
// `events` pass through as bytes, transfers on `bulk` are assembled and handed over whole.
namespace ketplus::runner::peer {

inline constexpr quint64 eventsBufferLimit = 256 * 1024;
inline constexpr qint64 runnerTransferTimeoutMs = 300'000;

class PeerChannelListener {
  public:
    virtual ~PeerChannelListener() = default;
    virtual void channelOpened(const QString& connectionId) = 0;
    // Once per opened connection, except after PeerChannel::close.
    virtual void channelLost(const QString& reason) = 0;
    virtual void frameReceived(protocol::Channel channel, const QByteArray& message) = 0;
    virtual void transferReceived(quint64 transferId, const QByteArray& bytes) = 0;
    virtual void eventsWritable() = 0;
    virtual void attemptFailed(const QString& reason) = 0;
    virtual void gaveUp(const QString& reason) = 0;
};

class PeerChannels final : public session::PeerChannel, public ConnectorEvents {
  public:
    PeerChannels(PeerClock& clock, PeerChannelListener& listener);
    // The connector this object answers for; set once, before any other call.
    void attach(PeerConnector& connector) { m_connector = &connector; }
    // The job requestConnection() asks for; empty when none.
    void setRequest(std::optional<PeerRequest> request) { m_request = std::move(request); }

    void send(protocol::Channel channel, const QByteArray& message) override;
    [[nodiscard]] bool eventsWritable() const override;
    void close(const QString& reason) override;
    void requestConnection() override;

    void connectionOpened(const QString& connectionId) override;
    void connectionLost(const QString& reason) override;
    void attemptFailed(const QString& reason) override;
    void gaveUp(const QString& reason) override;
    void received(Channel channel, const QByteArray& bytes, bool binary) override;
    void writable() override;

  private:
    void violation(const QString& reason);

    PeerClock& m_clock;
    PeerChannelListener& m_listener;
    PeerConnector* m_connector = nullptr;
    std::optional<PeerRequest> m_request;
    daemon::transport::FrameCodec m_codec{runnerTransferTimeoutMs};
    mutable bool m_saidNotWritable = false;
};

} // namespace ketplus::runner::peer
