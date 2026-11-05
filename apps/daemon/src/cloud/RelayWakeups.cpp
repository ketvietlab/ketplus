#include "cloud/RelayWakeups.h"
#include "cloud/ControlEndpoint.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QWebSocketHandshakeOptions>

#include <algorithm>
#include <utility>

namespace ketplus::daemon {
namespace {

const QString relayProtocol = QStringLiteral("ketplus.control.v2");
// A connection that lasted this long was healthy: its end starts the backoff over.
constexpr qint64 healthyMilliseconds = 30'000;

QUrl wakeUpUrl(const QUrl& controlBaseUrl) {
    QUrl url = controlEndpoint(controlBaseUrl, QStringLiteral("/v2/relay"));
    url.setScheme(url.scheme() == QStringLiteral("http") ? QStringLiteral("ws")
                                                         : QStringLiteral("wss"));
    return url;
}

} // namespace

RelayWakeups::RelayWakeups(const QUrl& controlBaseUrl, CredentialSource credential, QObject* parent)
    : QObject(parent), url_(wakeUpUrl(controlBaseUrl)), credential_(std::move(credential)) {
    retryTimer_.setSingleShot(true);
    connect(&retryTimer_, &QTimer::timeout, this, &RelayWakeups::connectNow);
    connect(&pingTimer_, &QTimer::timeout, this, [this] {
        if (!socket_)
            return;
        // The last ping went unanswered: the path is dead even if TCP has not noticed.
        if (awaitingPong_) {
            socket_->abort();
            return;
        }
        awaitingPong_ = true;
        socket_->ping();
    });
}

RelayWakeups::~RelayWakeups() {
    running_ = false;
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
    }
}

void RelayWakeups::setTimings(const int initialRetryMilliseconds,
                              const int maximumRetryMilliseconds, const int pingMilliseconds) {
    initialRetryMilliseconds_ = initialRetryMilliseconds;
    maximumRetryMilliseconds_ = maximumRetryMilliseconds;
    pingMilliseconds_ = pingMilliseconds;
}

void RelayWakeups::start() {
    if (running_)
        return;
    running_ = true;
    failures_ = 0;
    connectNow();
}

void RelayWakeups::stop() {
    running_ = false;
    ++generation_;
    retryTimer_.stop();
    pingTimer_.stop();
    if (socket_) {
        socket_->disconnect(this);
        socket_->abort();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    if (open_) {
        open_ = false;
        emit openChanged(false);
    }
}

void RelayWakeups::connectNow() {
    if (!running_ || socket_)
        return;
    const quint64 generation = ++generation_;
    credential_([self = QPointer<RelayWakeups>(this), generation](const QString& token) {
        if (!self || !self->running_ || generation != self->generation_ || self->socket_)
            return;
        if (token.isEmpty()) {
            self->scheduleRetry(self->nextBackoff());
            return;
        }
        auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, self);
        // The relay sends a few hundred bytes at most.
        socket->setMaxAllowedIncomingMessageSize(64 * 1024);
        self->socket_ = socket;
        self->openedAt_ = 0;
        connect(socket, &QWebSocket::connected, self, &RelayWakeups::opened);
        connect(socket, &QWebSocket::disconnected, self,
                [self = self.data(), socket] { self->closed(socket); });
        connect(socket, &QWebSocket::errorOccurred, self, [self = self.data(), socket] {
            if (socket->state() == QAbstractSocket::UnconnectedState)
                self->closed(socket);
        });
        connect(socket, &QWebSocket::textMessageReceived, self, &RelayWakeups::received);
        connect(socket, &QWebSocket::pong, self,
                [self = self.data()] { self->awaitingPong_ = false; });
        QNetworkRequest request(self->url_);
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        request.setRawHeader("X-KetPlus-Control-Version", "2");
        QWebSocketHandshakeOptions options;
        options.setSubprotocols({relayProtocol});
        socket->open(request, options);
    });
}

void RelayWakeups::opened() {
    if (!socket_)
        return;
    // A relay that does not speak this protocol would never send a wake-up.
    if (socket_->subprotocol() != relayProtocol) {
        socket_->abort();
        return;
    }
    open_ = true;
    openedAt_ = QDateTime::currentMSecsSinceEpoch();
    awaitingPong_ = false;
    pingTimer_.start(pingMilliseconds_);
    emit openChanged(true);
}

void RelayWakeups::closed(QWebSocket* socket) {
    if (socket != socket_)
        return;
    const int code = socket->closeCode();
    socket->disconnect(this);
    socket->deleteLater();
    socket_ = nullptr;
    pingTimer_.stop();
    const bool wasOpen = std::exchange(open_, false);
    if (wasOpen && QDateTime::currentMSecsSinceEpoch() - openedAt_ >= healthyMilliseconds)
        failures_ = 0;
    if (wasOpen)
        emit openChanged(false);
    if (!running_)
        return;
    if (wasOpen && code == 4003)
        emit authorizationRevoked();
    if (wasOpen && code == closeCredentialExpired)
        emit credentialExpired();
    scheduleRetry(nextBackoff());
}

bool RelayWakeups::send(const QJsonObject& message) {
    if (!socket_ || !open_ || socket_->bytesToWrite() > 256 * 1024)
        return false;
    const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (bytes.size() > 64 * 1024)
        return false;
    return socket_->sendTextMessage(QString::fromUtf8(bytes)) >= 0;
}

void RelayWakeups::received(const QString& text) {
    awaitingPong_ = false;
    const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type.startsWith("signal.") || type == "device.revoked")
        emit message(object);
    // ready: whatever arrived before this socket was never announced.
    // resync: the relay lost its own feed for a moment and may have missed some.
    if (type == QStringLiteral("ready") || type == QStringLiteral("resync") ||
        type == QStringLiteral("commands"))
        emit wake();
}

void RelayWakeups::scheduleRetry(const int milliseconds) {
    if (running_)
        retryTimer_.start(milliseconds);
}

int RelayWakeups::nextBackoff() {
    // Spread out so that every daemon a relay restart disconnects does not return at once.
    const int exponent = std::min(failures_++, 16);
    const qint64 base =
        std::min<qint64>(qint64(initialRetryMilliseconds_) << exponent, maximumRetryMilliseconds_);
    return int(base / 2 + QRandomGenerator::global()->bounded(base + 1));
}

} // namespace ketplus::daemon
