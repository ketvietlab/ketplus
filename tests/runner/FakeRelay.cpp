#include "FakeRelay.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingPins.h"
#include "pairing/PairingSignatures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>

#include <openssl/evp.h>

namespace runner_app_test {
namespace pairing = ketplus::runner::pairing;
namespace crypto = ketplus::control_crypto;

namespace {

using Reply = FakeRelay::Reply;

Reply refusal(const int status, const QString& code) {
    return {status, {{QStringLiteral("code"), code}, {QStringLiteral("message"), code}}, false};
}

QString text(const QByteArray& bytes) { return QString::fromLatin1(crypto::base64Url(bytes)); }

QByteArray bytes(const QJsonValue& value) {
    return pairing::decodeBase64Url(value.toString()).value_or(QByteArray());
}

} // namespace

QString randomSecret() {
    QByteArray random(32, Qt::Uninitialized);
    QRandomGenerator::global()->fillRange(reinterpret_cast<quint32*>(random.data()), 8);
    return text(random);
}

FakeRelay::FakeRelay(std::function<QDateTime()> now) : m_now(std::move(now)) {
    m_server.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
        while (QTcpSocket* socket = m_server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [this, socket] { serve(socket); });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, [this, socket] {
                m_input.remove(socket);
                socket->deleteLater();
            });
        }
    });
}

FakeRelay::~FakeRelay() {
    // Connections die before the members their handlers use.
    for (QTcpSocket* socket : m_server.findChildren<QTcpSocket*>()) {
        socket->disconnect();
        delete socket;
    }
    m_server.close();
}

QUrl FakeRelay::url() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
}

FakeRelay::Desktop& FakeRelay::desktop(const QString& machineId) {
    auto it = m_desktops.find(machineId);
    if (it == m_desktops.end()) {
        Desktop desktop;
        desktop.machineId = machineId;
        const crypto::Ed25519KeyPair signing = crypto::generateEd25519();
        desktop.signingPrivateKey = signing.privateKey;
        desktop.signingPublicKey = signing.publicKey;
        desktop.agreementPublicKey =
            crypto::publicKey(crypto::generatePrivateKey(EVP_PKEY_X25519), EVP_PKEY_X25519);
        it = m_desktops.insert(machineId, desktop);
    }
    return *it;
}

QByteArray FakeRelay::createToken(const QString& machineId,
                                  const std::optional<QString>& runnerId) {
    const Desktop& keys = desktop(machineId);
    Ticket ticket;
    ticket.id = QStringLiteral("ticket-%1").arg(m_nextId++);
    ticket.secret = randomSecret();
    ticket.machineId = machineId;
    ticket.forRunner = runnerId;
    tickets.insert(ticket.id, ticket);
    pairing::PairingTokenFields fields;
    fields.ticketId = ticket.id;
    fields.ticketSecret = ticket.secret;
    fields.workspaceId = workspaceId;
    fields.machineId = machineId;
    fields.machineKeyVersion = keys.keyVersion;
    fields.desktopFingerprint =
        pairing::desktopFingerprint(keys.signingPublicKey, keys.agreementPublicKey);
    fields.runnerId = runnerId;
    return pairing::PairingToken::create(fields).value().encode();
}

int FakeRelay::count(const QString& method, const QString& pathPart) const {
    int n = 0;
    for (const Request& request : requests)
        n += request.method == method.toLatin1() && request.path.contains(pathPart) ? 1 : 0;
    return n;
}

QStringList FakeRelay::secrets() const {
    QStringList all = accessTokens + refreshCredentials;
    for (const Ticket& ticket : tickets)
        all.append(ticket.secret);
    return all;
}

QJsonObject FakeRelay::desktopKeys(const QString& machineId) const {
    const Desktop& keys = m_desktops.value(machineId);
    return {{QStringLiteral("machineId"), machineId},
            {QStringLiteral("machineKeyVersion"), keys.keyVersion},
            {QStringLiteral("signingPublicKey"), text(keys.signingPublicKey)},
            {QStringLiteral("agreementPublicKey"), text(keys.agreementPublicKey)}};
}

void FakeRelay::claimTicket(Ticket& ticket, const Runner& runner) {
    const Desktop& keys = m_desktops.value(ticket.machineId);
    ticket.runnerId = runner.id;
    ticket.state = QStringLiteral("claimed");
    ticket.digest =
        pairing::pairingCode(
            {ticket.id, workspaceId, ticket.machineId, keys.keyVersion,
             pairing::desktopFingerprint(keys.signingPublicKey, keys.agreementPublicKey), runner.id,
             1, pairing::runnerFingerprint(runner.publicKey)})
            .digest;
}

Reply FakeRelay::registerRunner(const QJsonObject& body) {
    const QString ticketId = body.value(QStringLiteral("ticketId")).toString();
    if (!tickets.contains(ticketId))
        return refusal(404, QStringLiteral("ticket_not_found"));
    Ticket& ticket = tickets[ticketId];
    if (ticket.forRunner || body.value(QStringLiteral("ticketSecret")).toString() != ticket.secret)
        return refusal(404, QStringLiteral("ticket_not_found"));
    if (ticket.state != QStringLiteral("ticketed"))
        return refusal(409, QStringLiteral("ticket_used"));
    Runner runner;
    runner.publicKey = bytes(body.value(QStringLiteral("publicKey")));
    pairing::RegisterPayload payload{ticketId,
                                     workspaceId,
                                     ticket.machineId,
                                     body.value(QStringLiteral("keyVersion")).toInt(),
                                     pairing::runnerFingerprint(runner.publicKey),
                                     body.value(QStringLiteral("issuedAt")).toString()};
    if (pairing::verify(payload, bytes(body.value(QStringLiteral("signature"))),
                        {runner.publicKey, 1}, m_now()))
        return refusal(401, QStringLiteral("invalid_signature"));
    runner.id = QStringLiteral("runner-%1").arg(m_nextId++);
    runner.current = randomSecret();
    refreshCredentials.append(runner.current);
    runners.insert(runner.id, runner);
    lastRunnerId = runner.id;
    claimTicket(ticket, runner);
    return {201,
            {{QStringLiteral("runnerId"), runner.id},
             {QStringLiteral("workspaceId"), workspaceId},
             {QStringLiteral("refreshCredential"), runner.current},
             {QStringLiteral("desktop"), desktopKeys(ticket.machineId)},
             {QStringLiteral("pairState"), QStringLiteral("claimed")}},
            false};
}

Reply FakeRelay::challenge(const QJsonObject& body) {
    const QString runnerId = body.value(QStringLiteral("runnerId")).toString();
    if (!runners.contains(runnerId))
        return refusal(404, QStringLiteral("runner_not_found"));
    if (runners[runnerId].removed)
        return refusal(403, QStringLiteral("runner_removed"));
    const QString nonce = randomSecret();
    m_nonces.insert(nonce, runnerId);
    return {201,
            {{QStringLiteral("nonce"), nonce},
             {QStringLiteral("expiresAt"), pairing::issuedAtString(m_now().addSecs(60))}},
            false};
}

Reply FakeRelay::credential(const QJsonObject& body) {
    const QString runnerId = body.value(QStringLiteral("runnerId")).toString();
    const QString nonce = body.value(QStringLiteral("nonce")).toString();
    if (!runners.contains(runnerId))
        return refusal(401, QStringLiteral("invalid_access"));
    Runner& runner = runners[runnerId];
    if (runner.removed)
        return refusal(403, QStringLiteral("runner_removed"));
    if (m_nonces.take(nonce) != runnerId)
        return refusal(410, QStringLiteral("challenge_expired"));
    pairing::ChallengePayload payload{runnerId, body.value(QStringLiteral("keyVersion")).toInt(),
                                      nonce, body.value(QStringLiteral("issuedAt")).toString()};
    if (pairing::verify(payload, bytes(body.value(QStringLiteral("signature"))),
                        {runner.publicKey, 1}, m_now()))
        return refusal(401, QStringLiteral("invalid_signature"));
    // The current credential, or the previous one while the current was never presented.
    const QString presented = body.value(QStringLiteral("refreshCredential")).toString();
    if (presented == runner.current)
        runner.previous = runner.current;
    else if (presented.isEmpty() || presented != runner.previous)
        return refusal(401, QStringLiteral("invalid_access"));
    runner.current = randomSecret();
    refreshCredentials.append(runner.current);
    const QString token = randomSecret();
    accessTokens.append(token);
    m_bearers.insert(token, runnerId);
    return {201,
            {{QStringLiteral("accessToken"), token},
             {QStringLiteral("expiresAt"), pairing::issuedAtString(m_now().addSecs(15 * 60))},
             {QStringLiteral("refreshCredential"), runner.current}},
            false};
}

Reply FakeRelay::claim(const QString& runnerId, const QString& ticketId, const QJsonObject& body) {
    if (!tickets.contains(ticketId) || tickets[ticketId].forRunner != runnerId)
        return refusal(404, QStringLiteral("ticket_not_found"));
    Ticket& ticket = tickets[ticketId];
    if (body.value(QStringLiteral("ticketSecret")).toString() != ticket.secret)
        return refusal(404, QStringLiteral("ticket_not_found"));
    if (ticket.state != QStringLiteral("ticketed"))
        return refusal(409, QStringLiteral("ticket_used"));
    const Runner& runner = runners[runnerId];
    pairing::RegisterPayload payload{ticketId,
                                     workspaceId,
                                     ticket.machineId,
                                     1,
                                     pairing::runnerFingerprint(runner.publicKey),
                                     body.value(QStringLiteral("issuedAt")).toString()};
    if (pairing::verify(payload, bytes(body.value(QStringLiteral("signature"))),
                        {runner.publicKey, 1}, m_now()))
        return refusal(401, QStringLiteral("invalid_signature"));
    for (const Ticket& other : tickets) {
        if (other.runnerId == runnerId && other.machineId == ticket.machineId &&
            other.state == QStringLiteral("active"))
            return refusal(409, QStringLiteral("already_paired"));
    }
    claimTicket(ticket, runner);
    return {200,
            {{QStringLiteral("desktop"), desktopKeys(ticket.machineId)},
             {QStringLiteral("pairState"), QStringLiteral("claimed")}},
            false};
}

Reply FakeRelay::state(const QString& runnerId, const QString& ticketId) {
    if (!tickets.contains(ticketId) || tickets[ticketId].runnerId != runnerId)
        return refusal(404, QStringLiteral("ticket_not_found"));
    Ticket& ticket = tickets[ticketId];
    if (ticket.state == QStringLiteral("claimed") && ticket.polls++ >= approveAfterPolls) {
        const Desktop& keys = m_desktops.value(ticket.machineId);
        const auto sign = [&](const QByteArray& key, const QString& digest) {
            return text(
                pairing::signPayload(key, pairing::ApprovalPayload{ticket.id, digest}.bytes()));
        };
        switch (approval) {
        case Approval::Approve:
            ticket.state = QStringLiteral("active");
            ticket.approvalSignature = sign(keys.signingPrivateKey, ticket.digest);
            break;
        case Approval::BadSignature:
            ticket.state = QStringLiteral("active");
            ticket.approvalSignature = sign(crypto::generateEd25519().privateKey, ticket.digest);
            break;
        case Approval::WrongDigest:
            ticket.state = QStringLiteral("active");
            ticket.approvalSignature = sign(keys.signingPrivateKey, randomSecret());
            break;
        case Approval::Reject:
            ticket.state = QStringLiteral("rejected");
            break;
        case Approval::Expire:
            ticket.state = QStringLiteral("expired");
            break;
        case Approval::Revoke:
            ticket.state = QStringLiteral("revoked");
            break;
        case Approval::Forget:
            tickets.remove(ticketId);
            return refusal(404, QStringLiteral("ticket_not_found"));
        case Approval::Never:
            break;
        }
    }
    QJsonObject body{{QStringLiteral("ticketId"), ticket.id},
                     {QStringLiteral("pairState"), ticket.state}};
    if (ticket.state == QStringLiteral("active"))
        body.insert(QStringLiteral("approvalSignature"), ticket.approvalSignature);
    return {200, body, false};
}

Reply FakeRelay::heartbeat(const QString&) {
    return {200,
            {{QStringLiteral("compatibility"),
              QJsonObject{{QStringLiteral("capabilities"),
                           QJsonArray{QStringLiteral("runner.protocol/1")}}}},
             {QStringLiteral("disabled"), disabled},
             {QStringLiteral("serverTime"), pairing::issuedAtString(m_now())},
             {QStringLiteral("pending"),
              std::any_of(jobs.cbegin(), jobs.cend(),
                          [](const Job& job) { return job.state == QStringLiteral("queued"); })}},
            false};
}

Reply FakeRelay::revoke(const QString& runnerId, const QString& machineId) {
    bool paired = false;
    for (Ticket& ticket : tickets) {
        if (ticket.runnerId == runnerId && ticket.machineId == machineId &&
            (ticket.state == QStringLiteral("active") ||
             ticket.state == QStringLiteral("claimed"))) {
            ticket.state = QStringLiteral("revoked");
            paired = true;
        }
    }
    return paired ? Reply{204, {}, false} : refusal(403, QStringLiteral("not_paired"));
}

Reply FakeRelay::handle(const Request& request) {
    const QString& path = request.path;
    if (request.method == "POST" && path == QStringLiteral("/v2/runners:register"))
        return registerRunner(request.body);
    if (request.method == "POST" && path == QStringLiteral("/v2/runners/challenges"))
        return challenge(request.body);
    if (request.method == "POST" && path == QStringLiteral("/v2/runners/credentials"))
        return credential(request.body);

    const QString runnerId =
        request.authorization.startsWith("Bearer ")
            ? m_bearers.value(QString::fromLatin1(request.authorization.mid(7)))
            : QString();
    if (runnerId.isEmpty())
        return refusal(401, QStringLiteral("invalid_access"));
    if (runners[runnerId].removed)
        return refusal(403, QStringLiteral("runner_removed"));
    static const QRegularExpression ticketPath(
        QStringLiteral("^/v2/runner/pairing/tickets/([A-Za-z0-9_-]+)(/claim)?$"));
    static const QRegularExpression revokePath(
        QStringLiteral("^/v2/runner/pairs/([A-Za-z0-9_-]+)/revoke$"));
    if (const auto match = ticketPath.match(path); match.hasMatch()) {
        if (request.method == "POST" && !match.captured(2).isEmpty())
            return claim(runnerId, match.captured(1), request.body);
        if (request.method == "GET" && match.captured(2).isEmpty())
            return state(runnerId, match.captured(1));
    }
    if (std::optional<Reply> reply = handleJobs(runnerId, request))
        return *reply;
    if (request.method == "POST" && path == QStringLiteral("/v2/runner/heartbeat"))
        return heartbeat(runnerId);
    if (const auto match = revokePath.match(path); match.hasMatch() && request.method == "POST")
        return revoke(runnerId, match.captured(1));
    if (request.method == "POST" && path == QStringLiteral("/v2/runner:deregister")) {
        runners[runnerId].removed = true;
        return {204, {}, false};
    }
    return refusal(404, QStringLiteral("invalid_request"));
}

void FakeRelay::serve(QTcpSocket* socket) {
    QByteArray& input = m_input[socket];
    input += socket->readAll();
    while (true) {
        const qsizetype headerEnd = input.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QList<QByteArray> lines = input.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.front().trimmed().split(' ');
        Request request;
        request.method = requestLine.value(0);
        request.path = QUrl::fromPercentEncoding(requestLine.value(1));
        qsizetype length = 0;
        for (const QByteArray& line : lines.mid(1)) {
            const qsizetype colon = line.indexOf(':');
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "content-length")
                length = value.toLongLong();
            else if (name == "authorization")
                request.authorization = value;
        }
        if (input.size() < headerEnd + 4 + length)
            return;
        request.raw = input.mid(headerEnd + 4, length);
        request.body = QJsonDocument::fromJson(request.raw).object();
        input.remove(0, headerEnd + 4 + length);
        requests.append(request);

        std::optional<Reply> reply = intercept ? intercept(request) : std::nullopt;
        if (!reply)
            reply = handle(request);
        if (rewrite)
            rewrite(request, *reply);
        if (reply->drop) {
            socket->abort();
            return;
        }
        const QByteArray body = reply->status == 204 ? QByteArray()
                                : !reply->raw.isEmpty()
                                    ? reply->raw
                                    : QJsonDocument(reply->body).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(reply->status) +
                      " Status\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" +
                      body);
    }
}

} // namespace runner_app_test
