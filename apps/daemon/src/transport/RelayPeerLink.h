#pragma once

#include "transport/PeerLink.h"
#include <functional>

namespace ketplus::daemon::transport {
// One device's frames over the daemon's `/v2/relay` socket. Outgoing frames are wrapped as
// {"type":"frame","deviceId":…,"frame":…}; the socket hands the device's frames to deliver().
// Only event frames leave (events, event, gap, session.reset), each one text message of at most
// FrameCodec::messageLimit bytes; only subscribe and unsubscribe arrive, at most 4 KiB each.
// Everything runs on the daemon thread; the sender must not block.
class RelayPeerLink final : public PeerLink {
    Q_OBJECT
  public:
    // False when the socket cannot take the message now; the caller retries later.
    using Sender = std::function<bool(const QJsonObject&)>;
    static constexpr qsizetype clientFrameLimit = 4 * 1024;

    RelayPeerLink(QString device, Sender sender, QObject* parent = nullptr);
    static bool hostFrame(const QString& type);
    static bool clientFrame(const QString& type);
    QString device() const { return device_; }

    // A frame the device sent; false when refused, which the caller treats as a protocol error.
    bool deliver(const QJsonObject& frame);
    bool send(const Packet& packet) override;
    // The socket refuses messages above its own high-water mark, so nothing is buffered here.
    quint64 buffered(Channel) const override { return 0; }
    void close() override;

  private:
    void fail();
    QString device_;
    Sender sender_;
    bool closed_{false};
};
} // namespace ketplus::daemon::transport
