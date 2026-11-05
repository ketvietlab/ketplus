#pragma once

#include "transport/FrameCodec.h"
#include <QObject>

namespace ketplus::daemon::transport {
// All methods and signals live on the daemon thread. Implementations marshal native callbacks.
class PeerLink : public QObject {
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual bool send(const Packet& packet) = 0;
    virtual quint64 buffered(Channel channel) const = 0;
    virtual void close() = 0;

  signals:
    void opened();
    void received(Channel channel, const QByteArray& bytes, bool binary);
    void closed();
    void writable();
};
} // namespace ketplus::daemon::transport
