#include "FakeRelay.h"

#include "pairing/PairingSignatures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>

// The `/v2/relay` socket: wake-ups to runners and to the desktop, and signaling between them.
// The Relay forwards SDP and candidates without reading them.
namespace runner_app_test {

namespace {

constexpr qint64 startSpacingMs = 2'000;
constexpr int connectionsPerRunner = 4;

QString issued(const QDateTime& time) {
    return ketplus::runner::pairing::issuedAtString(time);
}

QByteArray json(const QJsonObject& message) {
    return QJsonDocument(message).toJson(QJsonDocument::Compact);
}

} // namespace

bool FakeRelay::upgrade(QTcpSocket* socket) {
    if (!socket->peek(14).startsWith("GET /v2/relay"))
        return false;
    QObject::disconnect(socket, nullptr, &m_server, nullptr);
    socket->disconnect();
    socket->setParent(nullptr);
    m_sockets->handleConnection(socket);
    return true;
}

void FakeRelay::opened(QWebSocket* socket) {
    const QByteArray authorization = socket->request().rawHeader("Authorization");
    const QByteArray bearer = authorization.startsWith("Bearer ") ? authorization.mid(7) : "";
    if (!machineToken.isEmpty() && bearer == machineToken.toLatin1()) {
        if (m_desktopSocket)
            m_desktopSocket->close(QWebSocketProtocol::CloseCodeNormal);
        m_desktopSocket = socket;
        QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                         [this](const QString& text) { fromDesktop(text); });
        QObject::connect(socket, &QWebSocket::disconnected, socket, [this, socket] {
            if (m_desktopSocket == socket)
                m_desktopSocket = nullptr;
            socket->deleteLater();
        });
        sendDesktop({{QStringLiteral("type"), QStringLiteral("ready")},
                     {QStringLiteral("role"), QStringLiteral("desktop")}});
        return;
    }
    const QString runnerId = m_bearers.value(QString::fromLatin1(bearer));
    if (runnerId.isEmpty() || runners.value(runnerId).removed) {
        if (strict)
            violate(QStringLiteral("socket opened without a valid runner or machine token"));
        socket->close(QWebSocketProtocol::CloseCode(4001));
        socket->deleteLater();
        return;
    }
    if (QWebSocket* previous = m_runnerSockets.take(runnerId)) {
        previous->disconnect();
        previous->close();
        previous->deleteLater();
    }
    m_runnerSockets.insert(runnerId, socket);
    QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                     [this, runnerId](const QString& text) { fromRunner(runnerId, text); });
    QObject::connect(socket, &QWebSocket::disconnected, socket, [this, runnerId, socket] {
        if (m_runnerSockets.value(runnerId) == socket)
            m_runnerSockets.remove(runnerId);
        // An attempt is forgotten when either side's socket closes.
        for (auto it = connections.begin(); it != connections.end();)
            it = it->runnerId == runnerId ? connections.erase(it) : std::next(it);
        socket->deleteLater();
    });
    sendRunner(runnerId, {{QStringLiteral("type"), QStringLiteral("ready")},
                          {QStringLiteral("role"), QStringLiteral("runner")}});
    for (const Job& job : std::as_const(jobs))
        if (job.state == QStringLiteral("queued") && (!strict || runnerMatches(runnerId, job)))
            sendRunner(runnerId, {{QStringLiteral("type"), QStringLiteral("job.available")},
                                  {QStringLiteral("jobId"), job.id}});
}

bool FakeRelay::runnerOnline(const QString& runnerId) const {
    return m_runnerSockets.contains(runnerId);
}

void FakeRelay::sendRunner(const QString& runnerId, const QJsonObject& message) {
    QWebSocket* socket = m_runnerSockets.value(runnerId);
    if (!socket)
        return;
    socketLog.append({{QStringLiteral("from"), QStringLiteral("relay")},
                      {QStringLiteral("to"), runnerId},
                      {QStringLiteral("message"), message}});
    socket->sendTextMessage(QString::fromUtf8(json(message)));
}

void FakeRelay::sendDesktop(const QJsonObject& message) {
    if (!m_desktopSocket)
        return;
    socketLog.append({{QStringLiteral("from"), QStringLiteral("relay")},
                      {QStringLiteral("to"), QStringLiteral("desktop")},
                      {QStringLiteral("message"), message}});
    m_desktopSocket->sendTextMessage(QString::fromUtf8(json(message)));
}

void FakeRelay::fromRunner(const QString& runnerId, const QString& text) {
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    const QJsonObject message = document.object();
    socketLog.append({{QStringLiteral("from"), runnerId},
                      {QStringLiteral("to"), QStringLiteral("relay")},
                      {QStringLiteral("message"), message}});
    if (!document.isObject()) {
        violate(QStringLiteral("runner socket text is not a JSON object"));
        return;
    }
    if (strict)
        checkMessage(QStringLiteral("runner"), message);
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("signal.start"))
        signalStart(runnerId, message);
    else if (type == QStringLiteral("signal.offer") || type == QStringLiteral("signal.candidate") ||
             type == QStringLiteral("signal.end"))
        relaySignal(runnerId, message);
}

void FakeRelay::fromDesktop(const QString& text) {
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    const QJsonObject message = document.object();
    socketLog.append({{QStringLiteral("from"), QStringLiteral("desktop")},
                      {QStringLiteral("to"), QStringLiteral("relay")},
                      {QStringLiteral("message"), message}});
    if (!document.isObject()) {
        violate(QStringLiteral("desktop socket text is not a JSON object"));
        return;
    }
    if (strict)
        checkMessage(QStringLiteral("desktop"), message);
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("signal.answer") || type == QStringLiteral("signal.candidate") ||
        type == QStringLiteral("signal.end"))
        relaySignal(QStringLiteral("desktop"), message);
}

// The checks run in the order of the contract; the first that fails answers.
void FakeRelay::signalStart(const QString& runnerId, const QJsonObject& message) {
    const auto error = [&](const QString& code) {
        sendRunner(runnerId, {{QStringLiteral("type"), QStringLiteral("signal.error")},
                              {QStringLiteral("code"), code}});
    };
    const qint64 now = m_now().toMSecsSinceEpoch();
    QList<qint64>& starts = m_starts[runnerId];
    const int open = int(std::count_if(connections.cbegin(), connections.cend(),
                                       [&](const Connection& c) { return c.runnerId == runnerId; }));
    if ((!starts.isEmpty() && now - starts.last() < startSpacingMs) ||
        open >= connectionsPerRunner) {
        error(QStringLiteral("rate_limited"));
        return;
    }
    starts.append(now);
    const QString jobId = message.value(QStringLiteral("jobId")).toString();
    const QString leaseId = message.value(QStringLiteral("leaseId")).toString();
    const Job* job = jobOf(jobId);
    if (!job || !activePair(runnerId, job->machineId)) {
        error(QStringLiteral("not_paired"));
        return;
    }
    const auto lease = leases.constFind(leaseId);
    if (lease == leases.cend() || !lease->open() || lease->runnerId != runnerId ||
        lease->jobId != jobId || lease->expiresAt <= m_now()) {
        error(QStringLiteral("lease_required"));
        return;
    }
    if (!m_desktopSocket || job->machineId != machineId) {
        error(QStringLiteral("host_offline"));
        return;
    }
    Connection connection;
    connection.id = QStringLiteral("pc_%1").arg(m_nextId++, 24, 10, QLatin1Char('0'));
    connection.runnerId = runnerId;
    connection.machineId = job->machineId;
    connection.jobId = jobId;
    connections.insert(connection.id, connection);
    sendRunner(runnerId, {{QStringLiteral("type"), QStringLiteral("signal.started")},
                          {QStringLiteral("connectionId"), connection.id},
                          {QStringLiteral("iceServers"), QJsonArray()},
                          {QStringLiteral("expiresAt"), issued(m_now().addSecs(60))}});
}

void FakeRelay::relaySignal(const QString& from, QJsonObject message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString id = message.value(QStringLiteral("connectionId")).toString();
    const auto found = connections.constFind(id);
    const bool fromDesktop = from == QStringLiteral("desktop");
    if (found == connections.cend() || (!fromDesktop && found->runnerId != from)) {
        // An attempt the Relay forgot, or another runner's: nothing is forwarded.
        if (type != QStringLiteral("signal.end"))
            violate(QStringLiteral("%1 for an unknown connection from %2").arg(type, from));
        return;
    }
    const Connection connection = *found;
    if (type == QStringLiteral("signal.end"))
        connections.remove(id);
    if (fromDesktop) {
        sendRunner(connection.runnerId, message);
        return;
    }
    if (type == QStringLiteral("signal.offer")) {
        message.insert(QStringLiteral("runnerId"), connection.runnerId);
        message.insert(QStringLiteral("iceServers"), QJsonArray());
    }
    sendDesktop(message);
}

} // namespace runner_app_test
