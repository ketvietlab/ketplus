#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QUrl>

// The runner's calls to the Relay, one blocking request at a time. A reply is either the
// expected success with its JSON body, a refusal with its code, or a network failure. Nothing
// here prints or logs: request bodies hold secrets.
namespace ketplus::runner::app {

struct RelayResult {
    enum class Kind {
        Ok,
        Refused,     // a 4xx with its refusal code ("unknown" when the body has none)
        Network,     // no HTTP answer: connection, timeout or TLS failure
        ServerError, // a 5xx, whatever its body: the request may or may not have taken effect
        BadReply,    // a success whose body is not what the contract says
    };
    Kind kind = Kind::Network;
    int status = 0;
    QString code;
    QJsonObject body;

    [[nodiscard]] bool ok() const { return kind == Kind::Ok; }
    // No usable answer: the request may have taken effect without the runner learning it.
    [[nodiscard]] bool lostAnswer() const {
        return kind == Kind::Network || kind == Kind::ServerError;
    }
    [[nodiscard]] bool refused(const char* refusal) const {
        return kind == Kind::Refused && code == QLatin1StringView(refusal);
    }
    // What a message may say about this result: the kind and the refusal code only.
    [[nodiscard]] QString describe() const;
};

class RelayClient {
  public:
    RelayClient(QUrl baseUrl, int timeoutMs);

    [[nodiscard]] bool configured() const { return m_baseUrl.isValid(); }

    [[nodiscard]] RelayResult registerRunner(const QJsonObject& body);
    [[nodiscard]] RelayResult createChallenge(const QString& runnerId);
    [[nodiscard]] RelayResult issueCredential(const QJsonObject& body);
    [[nodiscard]] RelayResult claim(const QString& ticketId, const QJsonObject& body,
                                    const QString& accessToken);
    [[nodiscard]] RelayResult pairingState(const QString& ticketId, const QString& accessToken);
    [[nodiscard]] RelayResult heartbeat(const QJsonObject& body, const QString& accessToken);
    [[nodiscard]] RelayResult revokePair(const QString& machineId, const QString& accessToken);
    [[nodiscard]] RelayResult deregister(const QString& accessToken);

  private:
    [[nodiscard]] RelayResult send(const QByteArray& method, const QString& path,
                                   const QJsonObject* body, const QString& accessToken,
                                   int successStatus);

    QUrl m_baseUrl;
    int m_timeoutMs;
    QNetworkAccessManager m_network;
};

// The Relay compiled into this build. A Debug build with the remote demo also takes
// KETPLUS_CONTROL_URL (https, or http on loopback), as the daemon does. Invalid when the
// build names none.
[[nodiscard]] QUrl compiledRelayUrl();

} // namespace ketplus::runner::app
