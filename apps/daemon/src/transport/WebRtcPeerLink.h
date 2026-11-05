#pragma once

#include "transport/PeerLink.h"
#include <QJsonArray>
#include <memory>

namespace ketplus::daemon::transport {
class WebRtcPeerLink final : public PeerLink {
    Q_OBJECT
  public:
    explicit WebRtcPeerLink(QObject* parent = nullptr);
    ~WebRtcPeerLink() override;
    bool answer(const QString& offer, const QJsonArray& iceServers);
    bool candidate(const QString& candidate, const QString& mid);
    bool send(const Packet& packet) override;
    quint64 buffered(Channel channel) const override;
    void close() override;

  signals:
    void localDescription(const QString& sdp);
    void localCandidate(const QString& candidate, const QString& mid);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ketplus::daemon::transport
