#pragma once
// A scripted peer for the relay frame link: no socket, no network.
#include "control/RelayFrameLink.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPointer>

namespace ketplus::remote {

class FakeFrameSocket final : public FrameSocket {
  public:
    using FrameSocket::FrameSocket;
    QUrl url;
    QString bearer;
    QList<QByteArray> sent;
    bool opening{false};
    bool refuseSend{false};
    void open(const QUrl& target, const QString& token) override {
        url = target;
        bearer = token;
        opening = true;
    }
    bool send(const QByteArray& text) override {
        if (refuseSend)
            return false;
        sent.append(text);
        return true;
    }
    void close() override { opening = false; }
    // The scripted relay side.
    void accept() { emit opened(); }
    void receive(const QByteArray& message) { emit text(message); }
    void receive(const QJsonObject& message) {
        emit text(QJsonDocument(message).toJson(QJsonDocument::Compact));
    }
    void hangUp(int code = 1006) { emit closed(code); }
    // A frame the relay forwards for a machine.
    void forward(const QString& machineId, const QJsonObject& frame) {
        receive(QJsonObject{{"type", "frame"}, {"machineId", machineId}, {"frame", frame}});
    }
    void drop(const QString& machineId, const QString& reason) {
        receive(
            QJsonObject{{"type", "frames.dropped"}, {"machineId", machineId}, {"reason", reason}});
    }
    // The frames this device sent for a machine, in order.
    [[nodiscard]] QList<QJsonObject> frames(const QString& machineId) const {
        QList<QJsonObject> out;
        for (const QByteArray& message : sent) {
            const auto object = QJsonDocument::fromJson(message).object();
            if (object.value("type") == "frame" && object.value("machineId") == machineId)
                out.append(object.value("frame").toObject());
        }
        return out;
    }
};

// Hands out sockets and remembers them, newest last.
struct FakeFrameSockets {
    QList<QPointer<FakeFrameSocket>> made;
    RelayFrameLink::SocketFactory factory() {
        return [this](QObject* parent) -> FrameSocket* {
            auto* socket = new FakeFrameSocket(parent);
            made.append(socket);
            return socket;
        };
    }
    [[nodiscard]] FakeFrameSocket* last() const {
        return made.isEmpty() ? nullptr : made.last().data();
    }
};

} // namespace ketplus::remote
