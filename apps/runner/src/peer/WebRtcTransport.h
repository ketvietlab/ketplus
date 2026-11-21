#pragma once

#include "peer/PeerPorts.h"

#include <QObject>
#include <QPointer>

namespace ketplus::daemon::transport {
class WebRtcPeerLink;
}

// PeerTransport over a WebRtcPeerLink in the offerer role.
namespace ketplus::runner::peer {

class WebRtcTransport final : public PeerTransport {
  public:
    explicit WebRtcTransport(PeerTransportEvents& events);
    ~WebRtcTransport() override;

    bool offer(const QJsonArray& iceServers) override;
    bool acceptAnswer(const QString& sdp, const QString& fingerprint) override;
    bool candidate(const QString& candidate, const QString& mid) override;
    bool send(Channel channel, const QByteArray& bytes, bool binary) override;
    [[nodiscard]] quint64 buffered(Channel channel) const override;
    void close() override;

  private:
    void release();

    QPointer<daemon::transport::WebRtcPeerLink> m_link;
};

class WebRtcTransportFactory final : public PeerTransportFactory {
  public:
    [[nodiscard]] std::unique_ptr<PeerTransport> create(PeerTransportEvents& events) override;
};

} // namespace ketplus::runner::peer
