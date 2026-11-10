#include "atlas/AtlasLiveTunnel.h"
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QWebSocketHandshakeOptions>
#include <algorithm>

namespace ketplus::daemon {
AtlasLiveTunnel::AtlasLiveTunnel(QByteArray key, QString channel, QHash<QString, quint16> origins,
                                 QObject* parent)
    : QObject(parent), codec_(std::move(key), std::move(channel), true),
      origins_(std::move(origins)) {
    socket_.setMaxAllowedIncomingMessageSize(65536);
    socket_.setMaxAllowedIncomingFrameSize(65536);
    connect(&socket_, &QWebSocket::binaryMessageReceived, this, &AtlasLiveTunnel::receive);
    connect(&socket_, &QWebSocket::textMessageReceived, this, [this](const QString& value) {
        const auto message = QJsonDocument::fromJson(value.toUtf8()).object();
        if (message.value("type") == "ready" && message.value("version").toInt() == 1)
            emit ready();
        else
            stop();
    });
    connect(&socket_, &QWebSocket::disconnected, this, &AtlasLiveTunnel::stop);
    connect(&socket_, &QWebSocket::errorOccurred, this, [this] { stop(); });
    connect(&socket_, &QWebSocket::bytesWritten, this, [this] {
        auto ids = streams_.keys();
        std::sort(ids.begin(), ids.end());
        std::rotate(ids.begin(), std::upper_bound(ids.begin(), ids.end(), drainAfter_), ids.end());
        for (const auto id : ids) {
            if (socket_.bytesToWrite() > 262144)
                break;
            drainAfter_ = id;
            drain(id);
        }
    });
}
void AtlasLiveTunnel::connectRelay(const QUrl& url, const QString& bearer) {
    if (url.scheme() != "wss" && !(url.scheme() == "ws" && url.host() == "127.0.0.1")) {
        stop();
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({"ketplus.atlas.v1"});
    socket_.open(request, options);
}
bool AtlasLiveTunnel::send(const QJsonObject& header, const QByteArray& body) {
    if (stopped_ || socket_.state() != QAbstractSocket::ConnectedState ||
        socket_.bytesToWrite() > 524288) {
        stop();
        return false;
    }
    const auto wire = codec_.seal({header, body});
    if (wire.isEmpty() || socket_.sendBinaryMessage(wire) != wire.size()) {
        stop();
        return false;
    }
    return true;
}
void AtlasLiveTunnel::remove(quint32 id) {
    const auto it = streams_.find(id);
    if (it == streams_.end())
        return;
    auto* tcp = it->socket;
    streams_.erase(it);
    tcp->disconnect(this);
    tcp->abort();
    tcp->deleteLater();
}
void AtlasLiveTunnel::stop() {
    if (stopped_)
        return;
    stopped_ = true;
    const auto ids = streams_.keys();
    for (const auto id : ids)
        remove(id);
    socket_.abort();
    emit closed();
}
void AtlasLiveTunnel::drain(quint32 id) {
    auto it = streams_.find(id);
    if (it == streams_.end() || it->sendCredit <= 0 || socket_.bytesToWrite() > 262144)
        return;
    const auto body = it->socket->read(qMin<qint64>(16384, it->sendCredit));
    if (!body.isEmpty()) {
        it->sendCredit -= body.size();
        if (!send({{"kind", "data"}, {"stream", int(id)}}, body))
            return;
    }
    // WebSocket writes can drain the last bytes after TCP EOF. The peer need not
    // grant more credit once it has received the complete body.
    it = streams_.find(id);
    if (it != streams_.end() && it->socket->state() == QAbstractSocket::UnconnectedState &&
        it->socket->bytesAvailable() == 0) {
        send({{"kind", "end"}, {"stream", int(id)}});
        remove(id);
    }
}
void AtlasLiveTunnel::receive(const QByteArray& wire) {
    AtlasTunnelFrame frame;
    if (!codec_.open(wire, &frame)) {
        stop();
        return;
    }
    const auto number = frame.header.value("stream").toInteger(-1);
    if (number < 1 || number > INT32_MAX ||
        double(number) != frame.header.value("stream").toDouble()) {
        stop();
        return;
    }
    const auto id = quint32(number);
    const auto kind = frame.header.value("kind").toString();
    if (kind == "open") {
        const auto origin = frame.header.value("origin").toString();
        if (id <= lastStream_ || streams_.size() >= 32 || !origins_.contains(origin)) {
            stop();
            return;
        }
        lastStream_ = id;
        auto* tcp = new QTcpSocket(this);
        tcp->setReadBufferSize(65536);
        streams_.insert(id, {tcp, 65536, 65536 - frame.body.size()});
        connect(tcp, &QTcpSocket::connected, this, [this, id, tcp, initial = frame.body] {
            if (!send({{"kind", "opened"}, {"stream", int(id)}}))
                return;
            if (!initial.isEmpty() && tcp->write(initial) != initial.size())
                stop();
        });
        connect(tcp, &QTcpSocket::readyRead, this, [this, id] { drain(id); });
        connect(tcp, &QTcpSocket::bytesWritten, this, [this, id](qint64 bytes) {
            auto it = streams_.find(id);
            if (it == streams_.end())
                return;
            it->receiveCredit += bytes;
            send({{"kind", "credit"}, {"stream", int(id)}, {"bytes", bytes}});
        });
        connect(tcp, &QTcpSocket::disconnected, this, [this, id] {
            // Drain the last bounded bytes before announcing EOF. Credits may arrive later.
            auto it = streams_.find(id);
            if (it == streams_.end())
                return;
            while (it->socket->bytesAvailable() && it->sendCredit > 0 &&
                   socket_.bytesToWrite() < 262144) {
                drain(id);
                it = streams_.find(id);
                if (it == streams_.end())
                    return;
            }
            if (it->socket->bytesAvailable() == 0) {
                send({{"kind", "end"}, {"stream", int(id)}});
                remove(id);
            }
        });
        connect(tcp, &QTcpSocket::errorOccurred, this,
                [this, id](QAbstractSocket::SocketError error) {
                    if (error != QAbstractSocket::RemoteHostClosedError) {
                        send({{"kind", "cancel"}, {"stream", int(id)}});
                        remove(id);
                    }
                });
        tcp->connectToHost(QHostAddress::LocalHost, origins_.value(origin));
        return;
    }
    auto it = streams_.find(id);
    if (it == streams_.end()) {
        if (id > lastStream_)
            stop();
        return;
    }
    if (kind == "data") {
        if (frame.body.isEmpty() || frame.body.size() > it->receiveCredit ||
            it->socket->state() != QAbstractSocket::ConnectedState) {
            stop();
            return;
        }
        it->receiveCredit -= frame.body.size();
        if (it->socket->write(frame.body) != frame.body.size())
            stop();
    } else if (kind == "credit") {
        const auto bytes = frame.header.value("bytes").toInteger(-1);
        if (!frame.body.isEmpty() || bytes < 1 || bytes > 65536 - it->sendCredit) {
            stop();
            return;
        }
        it->sendCredit += bytes;
        drain(id);
        it = streams_.find(id);
        if (it != streams_.end() && it->socket->state() == QAbstractSocket::UnconnectedState &&
            it->socket->bytesAvailable() == 0) {
            send({{"kind", "end"}, {"stream", int(id)}});
            remove(id);
        }
    } else if (kind == "cancel" || kind == "end")
        remove(id);
    else
        stop();
}
} // namespace ketplus::daemon
