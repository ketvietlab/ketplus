#include "ipc/LocalIpcServer.h"
#include "providers/ClaudeCredentials.h"

#include "cloud/CloudControlClient.h"
#include "protocol/DaemonProtocol.h"
#include "runtime/SessionHost.h"

#include "KetPlusProductVersion.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QVersionNumber>

#include <utility>

namespace ketplus::daemon {
namespace {

QString lockPathFor(const QString& serverName) {
    if (QFileInfo(serverName).isAbsolute()) {
        return serverName + QStringLiteral(".lock");
    }
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) {
        runtime = QDir::tempPath();
    }
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(serverName.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(runtime).filePath(QStringLiteral("ketplus-daemon-%1.lock").arg(digest));
}

// Past this many unsent bytes a client's next request is not read until it reads what it was
// sent. The answer to one request is at most a frame, so it always fits under
// maximumQueuedBytes; only events broadcast meanwhile can still overrun the queue.
constexpr qint64 readPauseBytes = protocol::maximumQueuedBytes - protocol::maximumFrameBytes - 4;

} // namespace

LocalIpcServer::LocalIpcServer(SessionHost* host, const QString& serverName,
                               const QByteArray& token, CloudControlClient* cloud, QObject* parent)
    : QObject(parent), host_(host), cloud_(cloud), server_(new QLocalServer(this)),
      serverName_(serverName), token_(token) {
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this, &LocalIpcServer::acceptConnections);
    connect(host_, &SessionHost::eventReady, this, &LocalIpcServer::broadcastEvent);
}

LocalIpcServer::~LocalIpcServer() { close(); }

bool LocalIpcServer::listen() {
    if (serverName_.isEmpty() || token_.isEmpty()) {
        error_ = QStringLiteral("The local endpoint and credential are required.");
        return false;
    }
    lock_ = std::make_unique<QLockFile>(lockPathFor(serverName_));
    if (!lock_->tryLock()) {
        error_ = QStringLiteral("Another KetPlus Daemon owns the local endpoint.");
        lock_.reset();
        return false;
    }
    if (server_->listen(serverName_)) {
        return true;
    }
    QLocalServer::removeServer(serverName_);
    if (server_->listen(serverName_)) {
        return true;
    }
    error_ = server_->errorString();
    lock_->unlock();
    lock_.reset();
    return false;
}

QString LocalIpcServer::errorString() const {
    return error_.isEmpty() ? server_->errorString() : error_;
}

QString LocalIpcServer::serverName() const { return serverName_; }

QString LocalIpcServer::fullServerName() const {
    return server_->isListening() ? server_->fullServerName() : serverName_;
}

void LocalIpcServer::close() {
    for (QLocalSocket* socket : connections_.keys()) {
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
    }
    connections_.clear();
    if (server_->isListening()) {
        server_->close();
        QLocalServer::removeServer(serverName_);
    }
    if (lock_) {
        lock_->unlock();
        lock_.reset();
    }
}

void LocalIpcServer::setHelloTimeout(const int milliseconds) {
    helloTimeoutMs_ = qMax(1, milliseconds);
}

void LocalIpcServer::setMaximumConnections(const int count) {
    maximumConnections_ = qMax(1, count);
}

int LocalIpcServer::connectionCount() const { return static_cast<int>(connections_.size()); }

void LocalIpcServer::acceptConnections() {
    while (QLocalSocket* socket = server_->nextPendingConnection()) {
        if (connections_.size() >= maximumConnections_) {
            // Not tracked: reply once, then let the peer see EOF. A client that never reads
            // cannot hold the slot because the socket is aborted after a short grace period.
            const QJsonObject synthetic{
                {QStringLiteral("message_type"), QStringLiteral("daemon.connection")}};
            const QByteArray frame = protocol::encodeFrame(
                protocol::failure(synthetic, QStringLiteral("connection_limit"),
                                  QStringLiteral("Too many local connections to the daemon.")));
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            socket->write(frame);
            socket->flush();
            socket->disconnectFromServer();
            const QPointer<QLocalSocket> guarded(socket);
            QTimer::singleShot(1'000, socket, [guarded] {
                if (guarded)
                    guarded->abort();
            });
            continue;
        }
        connections_.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readClient(socket); });
        // Queued so a flush() while answering does not read the next request inside that one.
        const QPointer<QLocalSocket> writing(socket);
        connect(
            socket, &QLocalSocket::bytesWritten, this,
            [this, writing] {
                if (writing && writing->bytesToWrite() <= readPauseBytes &&
                    writing->bytesAvailable() > 0) {
                    readClient(writing);
                }
            },
            Qt::QueuedConnection);
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            connections_.remove(socket);
            socket->deleteLater();
        });
        const QPointer<QLocalSocket> guarded(socket);
        QTimer::singleShot(helloTimeoutMs_, this, [this, guarded] {
            if (!guarded)
                return;
            const auto it = connections_.constFind(guarded.data());
            if (it == connections_.cend() || it->authenticated)
                return;
            // abort() emits disconnected synchronously, which removes the entry.
            guarded->abort();
        });
    }
}

void LocalIpcServer::readClient(QLocalSocket* socket) {
    auto it = connections_.find(socket);
    if (it == connections_.end()) {
        return;
    }
    while (socket->bytesAvailable() > 0) {
        // A client that sends many requests at once, as after a reconnect, is answered as
        // fast as it reads rather than disconnected for letting the answers pile up.
        if (socket->bytesToWrite() > readPauseBytes) {
            return;
        }
        if (it->expectedSize == 0) {
            if (socket->bytesAvailable() < 4) {
                return;
            }
            const QByteArray header = socket->read(4);
            it->expectedSize = (static_cast<quint32>(static_cast<uchar>(header[0])) << 24U) |
                               (static_cast<quint32>(static_cast<uchar>(header[1])) << 16U) |
                               (static_cast<quint32>(static_cast<uchar>(header[2])) << 8U) |
                               static_cast<quint32>(static_cast<uchar>(header[3]));
            if (it->expectedSize == 0 ||
                it->expectedSize > static_cast<quint32>(protocol::maximumFrameBytes)) {
                socket->abort();
                return;
            }
            it->frame.clear();
            it->frame.reserve(static_cast<qsizetype>(it->expectedSize));
        }
        const qsizetype remaining = static_cast<qsizetype>(it->expectedSize) - it->frame.size();
        it->frame.append(socket->read(qMin(socket->bytesAvailable(), remaining)));
        if (it->frame.size() != static_cast<qsizetype>(it->expectedSize)) {
            return;
        }
        const QByteArray frame = std::exchange(it->frame, {});
        it->expectedSize = 0;
        consumeFrame(socket, frame);
        it = connections_.find(socket);
        if (it == connections_.end()) {
            return;
        }
    }
}

void LocalIpcServer::consumeFrame(QLocalSocket* socket, const QByteArray& frame) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(frame, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        socket->abort();
        return;
    }
    const QJsonObject request = document.object();
    const protocol::ValidationResult validation = protocol::validateEnvelope(request);
    if (!validation.valid) {
        send(socket, protocol::failure(request, validation.errorCode, validation.message));
        socket->disconnectFromServer();
        return;
    }

    auto it = connections_.find(socket);
    if (it == connections_.end()) {
        return;
    }
    const QString type = request.value(QStringLiteral("message_type")).toString();
    if (!it->authenticated) {
        if (type != QStringLiteral("daemon.hello")) {
            send(socket, protocol::failure(request, QStringLiteral("authentication_required"),
                                           QStringLiteral("daemon.hello must be sent first.")));
            socket->disconnectFromServer();
            return;
        }
        const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
        if (payload.contains(QStringLiteral("agent_token"))) {
            const QString agentToken = payload.value(QStringLiteral("agent_token")).toString();
            if (!host_->isAgentToken(agentToken)) {
                send(socket, protocol::failure(request, QStringLiteral("authentication_failed"),
                                               QStringLiteral("The agent credential is invalid.")));
                socket->disconnectFromServer();
                return;
            }
            it->authenticated = true;
            it->agentToken = agentToken;
            it->clientId =
                QStringLiteral("agent:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            send(socket, protocol::response(request, {{QStringLiteral("ok"), true},
                                                      {QStringLiteral("protocol_version"),
                                                       protocol::currentVersion},
                                                      {QStringLiteral("agent"), true},
                                                      {QStringLiteral("daemon_version"),
                                                       QStringLiteral(KETPLUS_PRODUCT_VERSION)}}));
            return;
        }
        if (!tokenMatches(payload.value(QStringLiteral("token")).toString().toUtf8())) {
            send(socket, protocol::failure(request, QStringLiteral("authentication_failed"),
                                           QStringLiteral("The local credential is invalid.")));
            socket->disconnectFromServer();
            return;
        }
        it->authenticated = true;
        it->clientId = payload.value(QStringLiteral("client_id")).toString();
        if (it->clientId.isEmpty() || it->clientId.size() > 128) {
            it->clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        const int minor = protocol::negotiatedMinor(payload);
        it->activityGroups = minor >= 1 && payload.value("transcript_activity_groups").toBool();
        send(socket,
             protocol::response(
                 request,
                 {{QStringLiteral("ok"), true},
                  {QStringLiteral("protocol_version"), protocol::currentVersion},
                  {QStringLiteral("protocol_minor"), minor},
                  {QStringLiteral("features"), protocol::features(minor)},
                  {QStringLiteral("daemon_version"), QStringLiteral(KETPLUS_PRODUCT_VERSION)},
                  {QStringLiteral("client_id"), it->clientId},
                  {QStringLiteral("maximum_frame_bytes"),
                   static_cast<qint64>(protocol::maximumFrameBytes)},
                  {QStringLiteral("capabilities"),
                   QJsonArray{QStringLiteral("session.read"), QStringLiteral("session.control"),
                              QStringLiteral("session.delete"), QStringLiteral("transcript.read"),
                              QStringLiteral("approval.resolve"),
                              QStringLiteral("session.configuration.read"),
                              QStringLiteral("session.configuration.apply"),
                              QStringLiteral("session.execution_policy.apply"),
                              QStringLiteral("session.mode.apply"),
                              QStringLiteral("attachment.stage.begin"),
                              QStringLiteral("attachment.stage.chunk"),
                              QStringLiteral("attachment.stage.commit"),
                              QStringLiteral("attachment.stage.cancel"),
                              // MCP media is journaled as media_ref blocks resolved from the
                              // owner-only <state dir>/media store (same user; no IPC bytes).
                              QStringLiteral("media.local_store")}}}));
        return;
    }
    if (type == QStringLiteral("daemon.hello")) {
        send(socket,
             protocol::failure(request, QStringLiteral("already_authenticated"),
                               QStringLiteral("This connection is already authenticated.")));
        return;
    }
    if (shuttingDown_) {
        send(socket, protocol::failure(request, QStringLiteral("daemon_restarting"),
                                       QStringLiteral("The daemon is restarting for an upgrade.")));
        return;
    }
    if (!it->agentToken.isEmpty()) {
        send(socket, AgentBroker::isOperation(type)
                         ? protocol::response(
                               request, host_->handleAgentCommand(
                                            it->agentToken, type,
                                            request.value(QStringLiteral("payload")).toObject()))
                         : protocol::failure(request, QStringLiteral("forbidden"),
                                             QStringLiteral("Agent connections may only call "
                                                            "agents.* operations.")));
        return;
    }
    if (type == QStringLiteral("daemon.prepare_upgrade")) {
        const QVersionNumber current =
            QVersionNumber::fromString(QStringLiteral(KETPLUS_PRODUCT_VERSION));
        const QVersionNumber replacement =
            QVersionNumber::fromString(request.value(QStringLiteral("payload"))
                                           .toObject()
                                           .value(QStringLiteral("replacement_version"))
                                           .toString());
        if (replacement.isNull() || QVersionNumber::compare(replacement, current) <= 0) {
            send(socket, protocol::failure(
                             request, QStringLiteral("invalid_replacement_version"),
                             QStringLiteral("The replacement must be newer than this daemon.")));
            return;
        }
        if (host_->hasActiveSessions()) {
            send(socket,
                 protocol::failure(request, QStringLiteral("daemon_busy"),
                                   QStringLiteral("An active turn prevents daemon replacement.")));
            return;
        }
        shuttingDown_ = true;
        server_->close();
        send(socket, protocol::response(request, {{QStringLiteral("ok"), true}}));
        socket->flush();
        emit shutdownRequested();
        return;
    }
    if (type.startsWith(QStringLiteral("provider.claude.credentials."))) {
        if (!credentials_) {
            send(socket,
                 protocol::failure(
                     request, QStringLiteral("unsupported"),
                     QStringLiteral("Claude credential settings are unavailable on this daemon.")));
            return;
        }
        const QPointer<QLocalSocket> guard(socket);
        const QPointer<LocalIpcServer> server(this);
        // Retain only correlation fields in the asynchronous callback, never its secret payload.
        auto envelope = request;
        envelope.remove(QStringLiteral("payload"));
        credentials_->handle(
            type.mid(QStringLiteral("provider.claude.credentials.").size()),
            request.value(QStringLiteral("payload")).toObject(),
            [server, guard, envelope](const QJsonObject& result, const QString& error) {
                if (!server || !guard || !server->connections_.contains(guard))
                    return;
                if (!error.isEmpty()) {
                    server->send(guard, protocol::failure(
                                            envelope, QStringLiteral("credential_error"), error));
                    return;
                }
                auto payload = result;
                payload.insert(QStringLiteral("ok"), true);
                server->send(guard, protocol::response(envelope, payload));
            });
        return;
    }
    if (type == QStringLiteral("provider.models") ||
        type == QStringLiteral("provider.rate_limits") ||
        type == QStringLiteral("provider.runtime_status")) {
        const QPointer<QLocalSocket> guardedSocket(socket);
        host_->handleProviderQuery(request, [this, guardedSocket,
                                             request](const QJsonObject& result,
                                                      const QString& providerError) {
            if (guardedSocket.isNull() || !connections_.contains(guardedSocket)) {
                return;
            }
            send(guardedSocket,
                 providerError.isEmpty()
                     ? protocol::response(request, result)
                     : protocol::failure(request, QStringLiteral("provider_error"), providerError));
        });
        return;
    }
    if (type == QStringLiteral("session.fork")) {
        const QPointer<QLocalSocket> guardedSocket(socket);
        host_->forkSession(request, [this, guardedSocket, request](const QJsonObject& result) {
            if (guardedSocket.isNull() || !connections_.contains(guardedSocket))
                return;
            send(guardedSocket, protocol::response(request, result));
        });
        return;
    }
    if (SessionHost::isBackgroundTaskCommand(type)) {
        const QPointer<QLocalSocket> guardedSocket(socket);
        host_->handleCommandDeferred(
            request, it->clientId, [this, guardedSocket, request](const QJsonObject& result) {
                if (guardedSocket.isNull() || !connections_.contains(guardedSocket))
                    return;
                send(guardedSocket, protocol::response(request, result));
            });
        return;
    }
    if (cloud_ != nullptr && cloud_->handles(type)) {
        const QPointer<QLocalSocket> guardedSocket(socket);
        cloud_->handleLocal(
            type, request.value(QStringLiteral("payload")).toObject(),
            [this, guardedSocket, request](const QJsonObject& result, const QString& remoteError) {
                if (guardedSocket.isNull() || !connections_.contains(guardedSocket))
                    return;
                send(guardedSocket,
                     remoteError.isEmpty()
                         ? protocol::response(request, result)
                         : protocol::failure(request, QStringLiteral("remote_error"), remoteError));
            });
        return;
    }
    auto result = host_->handleCommand(request, it->clientId);
    if (it->activityGroups &&
        !request.value("payload").toObject().value("raw_activity_details").toBool() &&
        (type == "session.transcript_window" || type == "session.attach" ||
         type == "session.events_after" || type == "session.transcript_page")) {
        QJsonArray projected;
        QSet<QPair<qint64, QString>> grouped;
        for (const auto& value : result.value("events").toArray()) {
            const auto event = value.toObject();
            const auto summary = host_->activitySummaryEvent(event);
            if (summary.value("event_type") == "activity.group.summary")
                grouped.insert({event.value("turn_sequence").toInteger(),
                                event.value("payload").toObject().value("item_id").toString(
                                    event.value("payload")
                                        .toObject()
                                        .value("item")
                                        .toObject()
                                        .value("id")
                                        .toString())});
            projected.append(summary);
        }
        result.insert("events", projected);
        QJsonArray partial;
        for (const auto& value : result.value("partial_items").toArray())
            if (!grouped.contains({value.toObject().value("turn_sequence").toInteger(),
                                   value.toObject().value("item_id").toString()}))
                partial.append(value);
        result.insert("partial_items", partial);
        result.insert("activity_groups_version", 1);
    }
    send(socket, protocol::response(request, result));
}

void LocalIpcServer::send(QLocalSocket* socket, const QJsonObject& envelope) {
    const QByteArray frame = protocol::encodeFrame(envelope);
    if (frame.size() > protocol::maximumFrameBytes + 4 ||
        socket->bytesToWrite() + frame.size() > protocol::maximumQueuedBytes) {
        socket->abort();
        return;
    }
    socket->write(frame);
}

void LocalIpcServer::broadcastEvent(const QJsonObject& event) {
    QJsonObject envelope{
        {QStringLiteral("protocol_version"), protocol::currentVersion},
        {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("message_type"), QStringLiteral("session.event")},
        {QStringLiteral("session_id"), event.value(QStringLiteral("session_id"))},
        {QStringLiteral("session_generation"), event.value(QStringLiteral("session_generation"))},
        {QStringLiteral("cursor"), event.value(QStringLiteral("cursor"))},
        {QStringLiteral("turn_sequence"), event.value(QStringLiteral("turn_sequence"))},
        {QStringLiteral("sent_at"), event.value(QStringLiteral("sent_at"))},
        {QStringLiteral("payload"),
         QJsonObject{{QStringLiteral("event_type"), event.value(QStringLiteral("event_type"))},
                     {QStringLiteral("data"), event.value(QStringLiteral("payload"))}}}};
    // send() aborts a client whose queue is full, and abort() emits disconnected synchronously,
    // which removes that socket from connections_. Iterate a snapshot so the loop stays valid.
    const QList<QLocalSocket*> sockets = connections_.keys();
    for (QLocalSocket* socket : sockets) {
        const auto it = connections_.constFind(socket);
        if (it != connections_.cend() && it->authenticated && it->agentToken.isEmpty()) {
            auto outgoing = envelope;
            if (it->activityGroups) {
                const auto projected = host_->activitySummaryEvent(event);
                outgoing.insert("payload",
                                QJsonObject{{"event_type", projected.value("event_type")},
                                            {"data", projected.value("payload")}});
            }
            send(socket, outgoing);
        }
    }
}

bool LocalIpcServer::tokenMatches(const QByteArray& candidate) const {
    if (candidate.size() != token_.size()) {
        return false;
    }
    uchar difference = 0;
    for (qsizetype index = 0; index < token_.size(); ++index) {
        difference |= static_cast<uchar>(candidate[index]) ^ static_cast<uchar>(token_[index]);
    }
    return difference == 0;
}

} // namespace ketplus::daemon
