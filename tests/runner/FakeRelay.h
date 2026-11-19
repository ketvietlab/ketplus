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
#include <memory>
#include <optional>

class QWebSocket;
class QWebSocketServer;

// An in-process Relay on 127.0.0.1 that speaks the runner operations of the contract, checks
// every signature and the refresh credential rotation rule, and plays the desktop's part in a
// pairing. Tests change its answers through `intercept` and `rewrite`.
//
// It also serves the `/v2/relay` socket on the same port, to runners by their access token and,
// with `machineToken` set, to one desktop, whose pairing and job routes it then answers too.
// With `strict` set, every request and socket message is checked against the contract and each
// departure is recorded in `violations`.
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
        Reply() = default;
        Reply(const int status, QJsonObject body, const bool drop = false)
            : status(status), body(std::move(body)), drop(drop) {}
        int status = 200;
        QJsonObject body;
        bool drop = false; // close the connection without an answer, after handling
        QByteArray raw;    // sent as the body instead of `body` when not empty
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
        QJsonObject claim; // what runner.pairing.status shows once claimed
    };
    // A queued job and the lease a runner holds on it.
    struct Job {
        QString id;
        QString machineId = QStringLiteral("desktop-1");
        QString workspaceId = QStringLiteral("ws-1");
        QStringList runsOn{QStringLiteral("os:linux")};
        QString state = QStringLiteral("queued");
        QString reason; // when the Relay ended it
        int attempt = 0;
        QString submitKey; // jobs a desktop submitted
        QString runnerId;
        QString outcome;
        QDateTime queuedAt;
        QDateTime startedAt;
        QDateTime endedAt;
    };
    struct Lease {
        QString id;
        QString jobId;
        QString runnerId;
        QString acquireId;
        QDateTime expiresAt;
        QString startedAt;       // empty before job.start
        QString recordedOutcome; // finish outcome, or "released"
        QString recordedReason;
        bool requeued = false;
        [[nodiscard]] bool open() const { return recordedOutcome.isEmpty(); }
    };
    struct Runner {
        QString id;
        QByteArray publicKey;
        QString current;
        QString previous;
        bool removed = false;
        QString name;
        QString arch;
        QStringList labels; // user labels
    };
    // A signaling attempt between a runner and a desktop.
    struct Connection {
        QString id;
        QString runnerId;
        QString machineId;
        QString jobId;
    };

    explicit FakeRelay(std::function<QDateTime()> now);
    ~FakeRelay();

    [[nodiscard]] QUrl url() const;
    Desktop& desktop(const QString& machineId);
    // A token for a new runner, or for pairing `runnerId` with this desktop.
    [[nodiscard]] QByteArray createToken(const QString& machineId,
                                         const std::optional<QString>& runnerId = std::nullopt);
    [[nodiscard]] int count(const QString& method, const QString& pathPart) const;
    // The queue time of a job not taken ran out: it is `expired`, and the desktop hears
    // job.changed. False when the job is unknown or not queued.
    bool expire(const QString& jobId);
    // Every secret this Relay handed out or was handed, to scan output and files for.
    [[nodiscard]] QStringList secrets() const;

    QString workspaceId = QStringLiteral("ws-1");
    Approval approval = Approval::Approve;
    int approveAfterPolls = 0;
    bool disabled = false;
    QList<Request> requests;
    QList<Job> jobs;
    QMap<QString, Lease> leases;
    QString lastRunnerId;
    QMap<QString, Ticket> tickets;
    QMap<QString, Runner> runners;
    QStringList accessTokens;
    QStringList refreshCredentials;
    // Before the Relay's own handling; an answer here replaces it.
    std::function<std::optional<Reply>(const Request&)> intercept;
    // After it: may change the answer.
    std::function<void(const Request&, Reply&)> rewrite;

    // The desktop (FakeRelayDesktop.cpp) and the socket (FakeRelaySocket.cpp).
    QString machineToken; // the desktop's bearer token; empty serves no desktop
    QString machineId = QStringLiteral("desktop-1");
    // Socket messages in both directions as {"from", "message"}, "from" naming a runner id or
    // "desktop"; SDP and candidates included, for the test's own checks only.
    QList<QJsonObject> socketLog;
    QMap<QString, Connection> connections;
    [[nodiscard]] bool runnerOnline(const QString& runnerId) const;
    [[nodiscard]] bool desktopOnline() const { return m_desktopSocket != nullptr; }
    bool strict = false;
    QStringList violations;
    // Every desktop job route call, as "METHOD path".
    [[nodiscard]] int jobCalls(const QString& method, const QString& pathPart) const;

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
    // FakeRelayJobs.cpp.
    [[nodiscard]] std::optional<Reply> handleJobs(const QString& runnerId, const Request& request);
    [[nodiscard]] Reply acquire(const QString& runnerId, const QJsonObject& body);
    [[nodiscard]] Reply onLease(const QString& runnerId, const QString& leaseId,
                                const QString& action, const QJsonObject& body);
    [[nodiscard]] QJsonObject leaseJson(const Lease& lease) const;
    [[nodiscard]] Job* jobOf(const QString& jobId);
    // FakeRelayDesktop.cpp.
    [[nodiscard]] std::optional<Reply> handleDesktop(const Request& request);
    [[nodiscard]] Reply createTicket(const QJsonObject& body);
    [[nodiscard]] Reply ticketStatus(const QString& ticketId);
    [[nodiscard]] Reply approveTicket(const QString& ticketId, const QJsonObject& body);
    [[nodiscard]] Reply submitJob(const QJsonObject& body);
    [[nodiscard]] Reply cancelJob(const QString& jobId, const QJsonObject& body);
    [[nodiscard]] QJsonObject jobJson(const Job& job) const;
    [[nodiscard]] bool activePair(const QString& runnerId, const QString& machineId) const;
    [[nodiscard]] bool runnerMatches(const QString& runnerId, const Job& job) const;
    // The job's state changed: job.changed to the desktop.
    void jobChanged(const Job& job);
    // job.available to every runner whose socket is open and that could take the job.
    void announce(const Job& job);
    // FakeRelaySocket.cpp.
    bool upgrade(QTcpSocket* socket);
    void opened(QWebSocket* socket);
    void fromRunner(const QString& runnerId, const QString& text);
    void fromDesktop(const QString& text);
    void sendRunner(const QString& runnerId, const QJsonObject& message);
    void sendDesktop(const QJsonObject& message);
    void signalStart(const QString& runnerId, const QJsonObject& message);
    // A signal.offer, answer, candidate or end, sent on to the other side of its connection.
    void relaySignal(const QString& from, QJsonObject message);
    // FakeRelayRules.cpp: the contract's rules for a request and for a socket message.
    void checkRequest(const Request& request);
    void checkMessage(const QString& from, const QJsonObject& message);
    void violate(const QString& what);
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
    std::unique_ptr<QWebSocketServer> m_sockets;
    QHash<QString, QWebSocket*> m_runnerSockets; // by runner id
    QWebSocket* m_desktopSocket = nullptr;
    QHash<QString, QList<qint64>> m_starts; // signal.start times per runner, ms
};

[[nodiscard]] QString randomSecret();

} // namespace runner_app_test
