#pragma once

#include "pairing/PairingToken.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QTcpServer>
#include <QUrl>

#include <functional>
#include <optional>

// An in-process Relay on 127.0.0.1 that speaks the runner operations of the contract, checks
// every signature and the refresh credential rotation rule, and plays the desktop's part in a
// pairing. Tests change its answers through `intercept` and `rewrite`.
namespace runner_app_test {

class FakeRelay {
  public:
    struct Request {
        QByteArray method;
        QString path;
        QByteArray authorization;
        QJsonObject body;
        QByteArray raw;
    };
    struct Reply {
        int status = 200;
        QJsonObject body;
        bool drop = false; // close the connection without an answer, after handling
    };
    enum class Approval {
        Approve,
        Reject,
        Expire,
        Revoke,
        Forget,
        BadSignature,
        WrongDigest,
        Never
    };
    struct Desktop {
        QString machineId;
        int keyVersion = 1;
        QByteArray signingPrivateKey;
        QByteArray signingPublicKey;
        QByteArray agreementPublicKey;
    };
    struct Ticket {
        QString id;
        QString secret;
        QString machineId;
        std::optional<QString> forRunner;
        QString runnerId; // once claimed
        QString state = QStringLiteral("ticketed");
        QString digest;
        QString approvalSignature;
        int polls = 0;
    };
    struct Runner {
        QString id;
        QByteArray publicKey;
        QString current;
        QString previous;
        bool removed = false;
    };

    explicit FakeRelay(std::function<QDateTime()> now);
    ~FakeRelay();

    [[nodiscard]] QUrl url() const;
    Desktop& desktop(const QString& machineId);
    // A token for a new runner, or for pairing `runnerId` with this desktop.
    [[nodiscard]] QByteArray createToken(const QString& machineId,
                                         const std::optional<QString>& runnerId = std::nullopt);
    [[nodiscard]] int count(const QString& method, const QString& pathPart) const;
    // Every secret this Relay handed out or was handed, to scan output and files for.
    [[nodiscard]] QStringList secrets() const;

    QString workspaceId = QStringLiteral("ws-1");
    Approval approval = Approval::Approve;
    int approveAfterPolls = 0;
    bool disabled = false;
    QList<Request> requests;
    QString lastRunnerId;
    QMap<QString, Ticket> tickets;
    QMap<QString, Runner> runners;
    QStringList accessTokens;
    QStringList refreshCredentials;
    // Before the Relay's own handling; an answer here replaces it.
    std::function<std::optional<Reply>(const Request&)> intercept;
    // After it: may change the answer.
    std::function<void(const Request&, Reply&)> rewrite;

  private:
    [[nodiscard]] Reply handle(const Request& request);
    [[nodiscard]] Reply registerRunner(const QJsonObject& body);
    [[nodiscard]] Reply challenge(const QJsonObject& body);
    [[nodiscard]] Reply credential(const QJsonObject& body);
    [[nodiscard]] Reply claim(const QString& runnerId, const QString& ticketId,
                              const QJsonObject& body);
    [[nodiscard]] Reply state(const QString& runnerId, const QString& ticketId);
    [[nodiscard]] Reply heartbeat(const QString& runnerId);
    [[nodiscard]] Reply revoke(const QString& runnerId, const QString& machineId);
    [[nodiscard]] QJsonObject desktopKeys(const QString& machineId) const;
    // The ticket's pairing digest from the runner's key, as the desktop computes it.
    void claimTicket(Ticket& ticket, const Runner& runner);
    void serve(QTcpSocket* socket);

    std::function<QDateTime()> m_now;
    QTcpServer m_server;
    QMap<QString, Desktop> m_desktops;
    QHash<QString, QString> m_nonces;       // nonce -> runner id
    QHash<QString, QString> m_bearers;      // access token -> runner id
    QHash<QTcpSocket*, QByteArray> m_input; // unparsed bytes per connection
    int m_nextId = 1;
};

[[nodiscard]] QString randomSecret();

} // namespace runner_app_test
