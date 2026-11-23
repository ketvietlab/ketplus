#include "control/RelayFrameLink.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QPointer>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

namespace ketplus::remote {
namespace {
const QString relayProtocol = QStringLiteral("ketplus.control.v2");
constexpr int credentialRevokedCode = 4003;

QUrl socketUrl(const QUrl& origin) {
    QUrl url = origin;
    url.setScheme(origin.scheme() == QLatin1String("http") ? QStringLiteral("ws")
                                                           : QStringLiteral("wss"));
    url.setPath(QStringLiteral("/v2/relay"));
    url.setQuery({});
    return url;
}

class QtFrameSocket final : public FrameSocket {
  public:
    explicit QtFrameSocket(QObject* parent) : FrameSocket(parent) {
        socket_.setMaxAllowedIncomingMessageSize(RelayFrameLink::maximumReceiveBytes);
        connect(&socket_, &QWebSocket::connected, this, [this] {
            // A relay that does not speak this protocol would never carry a frame.
            if (socket_.subprotocol() != relayProtocol) {
                socket_.abort();
                return;
            }
            emit opened();
        });
        connect(&socket_, &QWebSocket::disconnected, this,
                [this] { emit closed(socket_.closeCode()); });
        connect(&socket_, &QWebSocket::textMessageReceived, this,
                [this](const QString& message) { emit text(message.toUtf8()); });
    }
    void open(const QUrl& url, const QString& bearer) override {
        QNetworkRequest request(url);
        request.setRawHeader("Authorization", "Bearer " + bearer.toUtf8());
        QWebSocketHandshakeOptions options;
        options.setSubprotocols({relayProtocol});
        socket_.open(request, options);
    }
    bool send(const QByteArray& message) override {
        return socket_.sendTextMessage(QString::fromUtf8(message)) >= 0;
    }
    void close() override { socket_.abort(); }

  private:
    QWebSocket socket_{QString(), QWebSocketProtocol::VersionLatest};
};
} // namespace

RelayFrameLink::SocketFactory RelayFrameLink::qtSockets() {
    return [](QObject* parent) -> FrameSocket* { return new QtFrameSocket(parent); };
}

RelayFrameLink::RelayFrameLink(const QUrl& origin, TokenSource token, SocketFactory factory,
                               Options options, QObject* parent)
    : QObject(parent), url_(socketUrl(origin)), token_(std::move(token)),
      factory_(std::move(factory)), options_(options) {
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &RelayFrameLink::connectNow);
}

RelayFrameLink::RelayFrameLink(const QUrl& origin, TokenSource token, SocketFactory factory,
                               QObject* parent)
    : RelayFrameLink(origin, std::move(token), std::move(factory), Options{}, parent) {}

RelayFrameLink::~RelayFrameLink() { stop(); }

void RelayFrameLink::setState(const State state) {
    if (state_ == state)
        return;
    state_ = state;
    emit stateChanged(state_);
}

void RelayFrameLink::start() {
    if (state_ != State::Stopped)
        return;
    failures_ = 0;
    connectNow();
}

void RelayFrameLink::stop() {
    ++attempt_;
    retry_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    setState(State::Stopped);
}

void RelayFrameLink::connectNow() {
    if (socket_)
        return;
    setState(State::Connecting);
    const quint64 attempt = ++attempt_;
    token_([self = QPointer<RelayFrameLink>(this), attempt](const QString& token,
                                                            const QString& error) {
        if (!self || attempt != self->attempt_ || self->socket_)
            return;
        if (!error.isEmpty() || token.isEmpty()) {
            self->scheduleRetry();
            return;
        }
        self->socket_ = self->factory_(self);
        connect(self->socket_, &FrameSocket::opened, self, [self = self.data()] {
            self->openedAt_ = QDateTime::currentMSecsSinceEpoch();
            self->setState(State::Open);
            emit self->opened();
        });
        connect(self->socket_, &FrameSocket::closed, self,
                [self = self.data()](const int code) { self->socketClosed(code); });
        connect(self->socket_, &FrameSocket::text, self,
                [self = self.data()](const QByteArray& message) { self->received(message); });
        self->socket_->open(self->url_, token);
    });
}

void RelayFrameLink::socketClosed(const int code) {
    if (!socket_)
        return;
    socket_->disconnect(this);
    socket_->deleteLater();
    socket_ = nullptr;
    if (openedAt_ > 0 &&
        QDateTime::currentMSecsSinceEpoch() - openedAt_ >= options_.healthyMilliseconds)
        failures_ = 0;
    openedAt_ = 0;
    if (code == credentialRevokedCode) {
        // No retry can succeed: this device is no longer paired.
        setState(State::Stopped);
        emit authorizationRevoked();
        return;
    }
    scheduleRetry();
}

void RelayFrameLink::scheduleRetry() {
    setState(State::Waiting);
    const int shift = qMin(failures_++, 16);
    retry_.start(int(qMin<qint64>(qint64(options_.initialBackoffMilliseconds) << shift,
                                  options_.maximumBackoffMilliseconds)));
}

bool RelayFrameLink::send(const QString& machineId, const QByteArray& frame) {
    if (state_ != State::Open || !socket_ || machineId.isEmpty())
        return false;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(frame, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QByteArray message =
        QJsonDocument(
            QJsonObject{{"type", "frame"}, {"machineId", machineId}, {"frame", document.object()}})
            .toJson(QJsonDocument::Compact);
    return message.size() <= maximumSendBytes && socket_->send(message);
}

void RelayFrameLink::received(const QByteArray& message) {
    if (message.size() > maximumReceiveBytes)
        return;
    const auto object = QJsonDocument::fromJson(message).object();
    const QString type = object.value("type").toString();
    const QString machineId = object.value("machineId").toString();
    if (machineId.isEmpty())
        return; // Wake-ups and anything else are not frames.
    if (type == QLatin1String("frames.dropped")) {
        emit dropped(machineId, object.value("reason").toString());
        return;
    }
    if (type != QLatin1String("frame") || !object.value("frame").isObject())
        return;
    emit frameReceived(
        machineId, QJsonDocument(object.value("frame").toObject()).toJson(QJsonDocument::Compact));
}

} // namespace ketplus::remote
