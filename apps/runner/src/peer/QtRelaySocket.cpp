#include "peer/QtRelaySocket.h"

#include <QJsonDocument>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

namespace ketplus::runner::peer {
namespace {

const QString relayProtocol = QStringLiteral("ketplus.control.v2");
constexpr qint64 messageLimit = 64 * 1024;
constexpr qint64 writeBacklogLimit = 256 * 1024;

} // namespace

QUrl QtRelaySocket::relayUrl(const QUrl& controlBaseUrl) {
    QUrl url = controlBaseUrl;
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    url.setPath(path + QStringLiteral("/v2/relay"));
    url.setScheme(url.scheme() == QLatin1StringView("http") ? QStringLiteral("ws")
                                                            : QStringLiteral("wss"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

int QtRelaySocket::refusedStatus(const QString& errorText) {
    // Qt names no status for a 401; it reports the authentication challenge it cannot answer.
    if (errorText.contains(QLatin1StringView("WWW-Authenticate"), Qt::CaseInsensitive))
        return 401;
    static const QRegularExpression pattern(QStringLiteral("http status code: (\\d{3})\\b"),
                                            QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(errorText);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QtRelaySocket::QtRelaySocket(const QUrl& controlBaseUrl, RelaySocketEvents& events,
                             const int pingMilliseconds)
    : m_url(relayUrl(controlBaseUrl)), m_events(events), m_pingMilliseconds(pingMilliseconds) {
    QObject::connect(&m_ping, &QTimer::timeout, &m_ping, [this] {
        if (!m_socket)
            return;
        if (m_awaitingPong) {
            m_socket->abort();
            return;
        }
        m_awaitingPong = true;
        m_socket->ping();
    });
}

QtRelaySocket::~QtRelaySocket() { close(); }

void QtRelaySocket::open(const QString& accessToken) {
    close();
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest);
    socket->setMaxAllowedIncomingMessageSize(messageLimit);
    m_socket = socket;
    m_connected = false;
    QObject::connect(socket, &QWebSocket::connected, socket, [this, socket] {
        if (socket != m_socket)
            return;
        if (socket->subprotocol() != relayProtocol) {
            socket->abort();
            return;
        }
        m_connected = true;
        m_awaitingPong = false;
        m_ping.start(m_pingMilliseconds);
        m_events.socketOpened();
    });
    QObject::connect(socket, &QWebSocket::disconnected, socket, [this, socket] { ended(socket); });
    QObject::connect(socket, &QWebSocket::errorOccurred, socket, [this, socket] {
        if (socket->state() == QAbstractSocket::UnconnectedState)
            ended(socket);
    });
    QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                     [this, socket](const QString& text) {
                         if (socket != m_socket)
                             return;
                         m_awaitingPong = false;
                         m_events.socketText(text);
                     });
    QObject::connect(socket, &QWebSocket::pong, socket, [this] { m_awaitingPong = false; });
    QNetworkRequest request(m_url);
    request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
    request.setRawHeader("X-KetPlus-Control-Version", "2");
    QWebSocketHandshakeOptions options;
    options.setSubprotocols({relayProtocol});
    socket->open(request, options);
}

void QtRelaySocket::ended(QWebSocket* socket) {
    if (socket != m_socket)
        return;
    const bool wasConnected = std::exchange(m_connected, false);
    // A connection that ended without a close frame reports a socket error; Qt still names 1000.
    const int code = socket->error() == QAbstractSocket::UnknownSocketError
                         ? int(socket->closeCode())
                         : int(QWebSocketProtocol::CloseCodeAbnormalDisconnection);
    const int status = refusedStatus(socket->errorString());
    m_ping.stop();
    m_socket.clear();
    socket->disconnect();
    socket->deleteLater();
    if (!wasConnected)
        m_events.socketRefused(status);
    else
        m_events.socketClosed(code);
}

bool QtRelaySocket::send(const QJsonObject& message) {
    if (!m_socket || !m_connected || m_socket->bytesToWrite() > writeBacklogLimit)
        return false;
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (bytes.size() > messageLimit)
        return false;
    return m_socket->sendTextMessage(QString::fromUtf8(bytes)) >= 0;
}

void QtRelaySocket::close() {
    m_ping.stop();
    m_connected = false;
    if (QWebSocket* socket = m_socket) {
        m_socket.clear();
        socket->disconnect();
        socket->abort();
        socket->deleteLater();
    }
}

} // namespace ketplus::runner::peer
