#pragma once
// A desktop daemon's Control v2 client against a fake Relay: an HTTP server on a fresh
// loopback port, in this process. Its answers are set per route; it records every request.
// Time is driven by the test: polls wait in `scheduled` until the test runs them.
#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingSignatures.h"
#include "providers/CodexAdapter.h"
#include "runner/RunnerService.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <memory>

namespace runner_test {

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;
namespace pairing = ketplus::runner::pairing;
namespace crypto = ketplus::control_crypto;

inline QString b64(const QByteArray& bytes) {
    return QString::fromLatin1(crypto::base64Url(bytes));
}

class FakeRelay final : public QTcpServer {
  public:
    struct Request {
        QByteArray method;
        QString path; // with its query
        QByteArray body;
        QByteArray authorization;
        [[nodiscard]] QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
    };
    struct Answer {
        int status = 200; // 0: the connection closes with no answer
        QByteArray body;
    };
    QList<Request> requests;

    FakeRelay() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket, buffer] { read(socket, *buffer); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    // The answer of "METHOD /path" (no query) until another is set; queued ones come first.
    void answer(const QString& route, int status, const QJsonValue& body = {}) {
        answers_[route] = {encode(status, body)};
    }
    void queue(const QString& route, int status, const QJsonValue& body = {}) {
        answers_[route].append(encode(status, body));
    }
    void refuse(const QString& route, int status, const QString& code) {
        answer(route, status, QJsonObject{{"code", code}, {"message", "synthetic refusal"}});
    }
    [[nodiscard]] int count(const QString& route) const {
        int found = 0;
        for (const Request& request : requests)
            found += key(request) == route ? 1 : 0;
        return found;
    }
    [[nodiscard]] QList<Request> of(const QString& route) const {
        QList<Request> found;
        for (const Request& request : requests) {
            if (key(request) == route)
                found.append(request);
        }
        return found;
    }

  private:
    static Answer encode(int status, const QJsonValue& body) {
        if (body.isUndefined() || body.isNull())
            return {status, {}};
        return {status, body.isArray() ? QJsonDocument(body.toArray()).toJson()
                                       : QJsonDocument(body.toObject()).toJson()};
    }
    static QString key(const Request& request) {
        const qsizetype mark = request.path.indexOf('?');
        return QString::fromLatin1(request.method) + ' ' +
               (mark < 0 ? request.path : request.path.left(mark));
    }
    void read(QTcpSocket* socket, QByteArray& buffer) {
        buffer.append(socket->readAll());
        if (buffer.size() > 1024 * 1024)
            qFatal("Fake relay request exceeds bound");
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> start = lines.first().trimmed().split(' ');
        Request request{start.value(0), QString::fromLatin1(start.value(1)), {}, {}};
        qint64 length = 0;
        for (const QByteArray& line : lines) {
            const QByteArray lower = line.toLower();
            if (lower.startsWith("content-length:"))
                length = line.mid(15).trimmed().toLongLong();
            if (lower.startsWith("authorization:"))
                request.authorization = line.mid(14).trimmed();
        }
        if (buffer.size() - headerEnd - 4 < length)
            return;
        request.body = buffer.mid(headerEnd + 4, length);
        requests.append(request);
        QList<Answer>& queued = answers_[key(request)];
        const Answer reply = queued.isEmpty()
                                 ? Answer{404, R"({"code":"not_found","message":"no route"})"}
                                 : (queued.size() > 1 ? queued.takeFirst() : queued.first());
        if (reply.status == 0) {
            socket->abort();
            return;
        }
        socket->write("HTTP/1.1 " + QByteArray::number(reply.status) +
                      " Synthetic\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" +
                      reply.body);
        socket->disconnectFromHost();
    }
    QHash<QString, QList<Answer>> answers_;
};

class LoopbackOnly final : public QNetworkAccessManager {
  public:
    explicit LoopbackOnly(quint16 port) : port_(port) { setProxy(QNetworkProxy::NoProxy); }

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override {
        if (request.url().scheme() != "http" || request.url().host() != "127.0.0.1" ||
            request.url().port() != port_)
            qFatal("Test transport attempted to leave its loopback relay");
        return QNetworkAccessManager::createRequest(operation, request, outgoing);
    }

  private:
    quint16 port_;
};

// A runner node: its key and the claim it signs.
struct FakeRunner {
    crypto::Ed25519KeyPair key = crypto::generateEd25519();
    QString id = QStringLiteral("runner-a");
    int keyVersion = 1;
    QString name = QStringLiteral("build-node");

    [[nodiscard]] QJsonObject claim(const QString& ticketId, const QString& issuedAt,
                                    const QString& workspace = "workspace-synthetic",
                                    const QString& machine = "machine-synthetic") const {
        const pairing::RegisterPayload payload{
            ticketId, workspace, machine, keyVersion, pairing::runnerFingerprint(key.publicKey),
            issuedAt};
        return {{"ticketId", ticketId},
                {"pairState", "claimed"},
                {"runnerId", id},
                {"publicKey", b64(key.publicKey)},
                {"keyVersion", keyVersion},
                {"name", name},
                {"os", "linux"},
                {"arch", "arm64"},
                {"labels", QJsonArray{"linux", "arm64"}},
                {"issuedAt", issuedAt},
                {"signature", b64(pairing::signPayload(key.privateKey, payload.bytes()))}};
    }
};

inline const QString ticketId = QStringLiteral("ticket-synthetic");
inline const QString ticketSecret = QStringLiteral("synthetic-ticket-secret-0123456789abcdef");
inline const QString createRoute = QStringLiteral("POST /v2/runner-pairing/tickets");
inline const QString statusRoute = QStringLiteral("GET /v2/runner-pairing/tickets/") + ticketId;
inline const QString approveRoute =
    QStringLiteral("POST /v2/runner-pairing/tickets/") + ticketId + "/approve";
inline const QString rejectRoute =
    QStringLiteral("POST /v2/runner-pairing/tickets/") + ticketId + "/reject";
inline const QString listRoute = QStringLiteral("GET /v2/runners");

struct Fixture {
    QTemporaryDir directory;
    LocalJournal journal{directory.filePath("journal.sqlite")};
    CodexAdapter provider{QStringLiteral("/nonexistent/synthetic-provider")};
    SessionHost host{&journal, &provider};
    FakeRelay relay;
    std::unique_ptr<LoopbackOnly> network;
    std::unique_ptr<CloudControlClient> client;
    QDateTime now{QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC};
    QList<std::pair<int, std::function<void()>>> scheduled;
    QList<QJsonObject> events;
    QList<QJsonObject> replies;

    Fixture() {
        if (!relay.listen(QHostAddress::LocalHost, 0))
            qFatal("Fake relay cannot listen");
        network = std::make_unique<LoopbackOnly>(relay.serverPort());
        client = std::make_unique<CloudControlClient>(&journal, &host);
        Access::configure(*client, *network,
                          QUrl(QStringLiteral("http://127.0.0.1:%1").arg(relay.serverPort())));
        Access::machineKeyVersion(*client, 1);
        offerRunners(true);
        service().setTime([this] { return now; },
                          [this](int delay, std::function<void()> task) {
                              scheduled.append({delay, std::move(task)});
                          });
        QObject::connect(&service(), &runner::RunnerService::pairingChanged,
                         [this](const QJsonObject& event) { events.append(event); });
    }
    runner::RunnerService& service() { return client->runners(); }
    void offerRunners(bool offered) {
        QJsonArray capabilities{"transport.webrtc/1"};
        if (offered)
            capabilities.append("runner.relay/1");
        Access::heartbeatAnswer(*client,
                                {{"compatibility", QJsonObject{{"capabilities", capabilities}}}});
    }
    // Calls the local IPC operation as LocalIpcServer does and waits for its answer.
    QJsonObject call(const QString& type, const QJsonObject& payload = {}) {
        QJsonObject result;
        bool done = false;
        client->handleLocal(type, payload, [&](const QJsonObject& value, const QString& error) {
            result = value;
            if (!error.isEmpty())
                result.insert("transport_error", error);
            done = true;
        });
        if (!QTest::qWaitFor([&] { return done; }, 5000))
            result = {{"ok", false}, {"error_code", "test_timeout"}};
        replies.append(result);
        return result;
    }
    // Runs the next poll after its delay and waits until the daemon has handled the answer:
    // a handled poll either schedules the next one or reports a change.
    int runPoll() {
        if (scheduled.isEmpty())
            return -1;
        auto [delay, task] = scheduled.takeFirst();
        now = now.addMSecs(delay);
        const auto pending = scheduled.size();
        const auto reported = events.size();
        task();
        // A poll of a pairing that already ended does neither; the wait then times out.
        (void)QTest::qWaitFor(
            [&] { return scheduled.size() > pending || events.size() > reported; }, 3000);
        return delay;
    }
    [[nodiscard]] QJsonObject keys() { return Access::crypto(*client).publicKeys(); }
    [[nodiscard]] QByteArray key(const char* name) {
        return *pairing::decodeBase64Url(keys().value(QLatin1StringView(name)).toString());
    }
    [[nodiscard]] QString desktopFingerprint() {
        return pairing::desktopFingerprint(key("signingPublicKey"), key("agreementPublicKey"));
    }
    [[nodiscard]] pairing::PairingCode expectedCode(const FakeRunner& runner) {
        return pairing::pairingCode({ticketId, "workspace-synthetic", "machine-synthetic", 1,
                                     desktopFingerprint(), runner.id, runner.keyVersion,
                                     pairing::runnerFingerprint(runner.key.publicKey)});
    }
    // A ticket the Relay grants, created through IPC: answers the create reply.
    QJsonObject createTicket(const QJsonObject& payload = {}) {
        relay.answer(createRoute, 201,
                     QJsonObject{{"ticketId", ticketId},
                                 {"ticketSecret", ticketSecret},
                                 {"expiresAt", now.addSecs(3600).toString(Qt::ISODateWithMs)}});
        relay.answer(statusRoute, 200,
                     QJsonObject{{"ticketId", ticketId}, {"pairState", "ticketed"}});
        return call("runner.pairing.create", payload);
    }
    // Creates a ticket and lets `runner` claim it; answers the status with its code.
    QJsonObject claimed(const FakeRunner& runner) {
        createTicket();
        relay.answer(statusRoute, 200, runner.claim(ticketId, issued(now)));
        runPoll();
        return call("runner.pairing.status", {{"ticket_id", ticketId}});
    }
    static QString issued(const QDateTime& time) { return pairing::issuedAtString(time); }
    [[nodiscard]] static QByteArray storedPins() { return CredentialStore().read("runner-pins"); }
};

} // namespace runner_test
