#pragma once

#include "peer/PeerPorts.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QWebSocket;

// RelaySocket over QWebSocket: the `/v2/relay` URL with the control subprotocol, a bearer
// token, and a ping that aborts a path that stopped answering.
namespace ketplus::runner::peer {

class QtRelaySocket final : public RelaySocket {
  public:
    // `controlBaseUrl` is the Relay's https (or, in tests, http) base URL.
    QtRelaySocket(const QUrl& controlBaseUrl, RelaySocketEvents& events,
                  int pingMilliseconds = 25'000);
    ~QtRelaySocket() override;

    void open(const QString& accessToken) override;
    bool send(const QJsonObject& message) override;
    void close() override;

    // The HTTP status a refused upgrade names in the socket's error text, or 0.
    [[nodiscard]] static int refusedStatus(const QString& errorText);
    [[nodiscard]] static QUrl relayUrl(const QUrl& controlBaseUrl);

  private:
    void ended(QWebSocket* socket);

    QUrl m_url;
    RelaySocketEvents& m_events;
    int m_pingMilliseconds;
    QPointer<QWebSocket> m_socket;
    bool m_connected = false;
    bool m_awaitingPong = false;
    QTimer m_ping;
};

} // namespace ketplus::runner::peer
