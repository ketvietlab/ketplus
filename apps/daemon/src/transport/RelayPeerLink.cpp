#include "transport/RelayPeerLink.h"
#include <QJsonDocument>
#include <QMetaObject>

namespace ketplus::daemon::transport {
RelayPeerLink::RelayPeerLink(QString device, Sender sender, QObject* parent)
    : PeerLink(parent), device_(std::move(device)), sender_(std::move(sender)) {}

bool RelayPeerLink::hostFrame(const QString& type) {
    return type == "events" || type == "event" || type == "gap" || type == "session.reset";
}

bool RelayPeerLink::clientFrame(const QString& type) {
    return type == "subscribe" || type == "unsubscribe";
}

bool RelayPeerLink::deliver(const QJsonObject& frame) {
    if (closed_ || !clientFrame(frame.value("type").toString()))
        return false;
    const auto bytes = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (bytes.size() > clientFrameLimit)
        return false;
    emit received(Channel::Control, bytes, false);
    return true;
}

bool RelayPeerLink::send(const Packet& packet) {
    if (closed_)
        return false;
    QJsonParseError error{};
    const auto frame = QJsonDocument::fromJson(packet.bytes, &error).object();
    if (packet.binary || packet.bytes.size() > FrameCodec::messageLimit ||
        error.error != QJsonParseError::NoError || !hostFrame(frame.value("type").toString())) {
        // A frame this path cannot carry would block the queue forever.
        fail();
        return false;
    }
    const QJsonObject message{{"type", "frame"}, {"deviceId", device_}, {"frame", frame}};
    if (QJsonDocument(message).toJson(QJsonDocument::Compact).size() > FrameCodec::messageLimit) {
        // The socket refuses it every time; the limit counts the envelope too.
        fail();
        return false;
    }
    return sender_ && sender_(message);
}

void RelayPeerLink::close() { closed_ = true; }

void RelayPeerLink::fail() {
    if (closed_)
        return;
    closed_ = true;
    // Queued: the caller is inside its own send loop.
    QMetaObject::invokeMethod(this, [this] { emit closed(); }, Qt::QueuedConnection);
}
} // namespace ketplus::daemon::transport
