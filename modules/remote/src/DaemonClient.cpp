#include "DaemonClient.h"
#include "DaemonCredentialFile.h"
#include "DaemonPaths.h"

#include "KetPlusProductVersion.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QVersionNumber>

#include <algorithm>
#include <utility>

namespace ketplus::remote {
namespace {

constexpr int protocolVersion = 0;
constexpr int supportedProtocolMinor = 1;
constexpr qsizetype maximumFrameBytes = 1024 * 1024;

// Reads whose answer is the same for every copy sent together. Copies waiting to be sent are
// merged into one request; writes are never merged.
bool mergesWhileQueued(const QString& type) {
    static const QSet<QString> reads{QStringLiteral("session.list"),
                                     QStringLiteral("session.pending_approvals"),
                                     QStringLiteral("session.configuration.read"),
                                     QStringLiteral("provider.models"),
                                     QStringLiteral("provider.rate_limits"),
                                     QStringLiteral("provider.runtime_status"),
                                     QStringLiteral("remote.status"),
                                     QStringLiteral("remote.devices"),
                                     QStringLiteral("remote.pairing.status")};
    return reads.contains(type);
}

// Reads that are safe to send again when the connection dropped before their answer. A write in
// flight may already have happened, and the daemon's result cache is not written atomically with
// it, so its caller is told instead of having it repeated.
bool replaysAfterDisconnect(const QString& type) {
    static const QSet<QString> replayable{QStringLiteral("session.list"),
                                          QStringLiteral("session.attach"),
                                          QStringLiteral("session.events_after"),
                                          QStringLiteral("session.transcript_page"),
                                          QStringLiteral("session.transcript_window"),
                                          QStringLiteral("session.transcript_large_event"),
                                          QStringLiteral("session.activity_group_page"),
                                          QStringLiteral("session.pending_approvals"),
                                          QStringLiteral("session.configuration.read"),
                                          QStringLiteral("session.worktrees"),
                                          QStringLiteral("session.resource.resolve"),
                                          QStringLiteral("session.output.read"),
                                          QStringLiteral("session.media.read"),
                                          QStringLiteral("session.task.output"),
                                          QStringLiteral("provider.models"),
                                          QStringLiteral("provider.rate_limits"),
                                          QStringLiteral("provider.runtime_status"),
                                          QStringLiteral("remote.status"),
                                          QStringLiteral("remote.devices"),
                                          QStringLiteral("remote.pairing.status"),
                                          QStringLiteral("remote.audit")};
    return replayable.contains(type);
}

} // namespace

DaemonClient::DaemonClient(QObject* parent)
    : QObject(parent), socketPath_(defaultSocketPath()), tokenPath_(defaultTokenPath()),
      statePath_(defaultStatePath()),
      clientId_(
          QStringLiteral("desktop-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {
    // Keep transport backpressure active even while application dispatch is paused.
    socket_.setReadBufferSize(maximumFrameBytes + 4);
    reconnectTimer_.setSingleShot(true);
    connect(&reconnectTimer_, &QTimer::timeout, this, &DaemonClient::connectToDaemon);
    stableTimer_.setSingleShot(true);
    stableTimer_.setInterval(stableConnectionMs);
    connect(&stableTimer_, &QTimer::timeout, this, [this] { reconnectAttempt_ = 0; });
    connect(&socket_, &QLocalSocket::connected, this, &DaemonClient::authenticate);
    connect(&socket_, &QLocalSocket::readyRead, this, &DaemonClient::readFrames);
    connect(&socket_, &QLocalSocket::disconnected, this, [this] {
        const bool wasReady = ready_;
        stableTimer_.stop();
        launched_ = false;
        setReady(false);
        buffer_.clear();
        expectedSize_ = 0;
        QList<QueuedCall> interrupted;
        for (QueuedCall& call : std::exchange(pending_, {})) {
            if (call.type.startsWith(QStringLiteral("storage."))) {
                if (call.callback)
                    call.callback(
                        {}, QStringLiteral(
                                "Daemon disconnected. Reload storage settings before retrying."));
            } else if (call.type.startsWith(QStringLiteral("provider.claude.credentials."))) {
                if (call.callback)
                    call.callback(
                        {}, QStringLiteral("Daemon disconnected. Check the saved credential state "
                                           "before retrying."));
            } else if (replaysAfterDisconnect(call.type)) {
                interrupted.append(std::move(call));
            } else if (call.type != QStringLiteral("daemon.hello") &&
                       call.type != QStringLiteral("daemon.prepare_upgrade") && call.callback) {
                call.callback({}, QStringLiteral("Daemon disconnected before confirming the "
                                                 "request. Check its result before retrying."));
            }
        }
        std::sort(interrupted.begin(), interrupted.end(),
                  [](const QueuedCall& left, const QueuedCall& right) {
                      return left.sequence < right.sequence;
                  });
        // Calls that were sent go first, in the order they were made; then the ones waiting.
        const QList<QueuedCall> waiting = std::exchange(queued_, {});
        for (QueuedCall& call : interrupted) {
            enqueue(std::move(call));
        }
        for (QueuedCall call : waiting) {
            if (call.type.startsWith(QStringLiteral("storage."))) {
                if (call.callback)
                    call.callback(
                        {}, QStringLiteral(
                                "Daemon disconnected. Reload storage settings before retrying."));
                continue;
            }
            if (call.type.startsWith(QStringLiteral("provider.claude.credentials."))) {
                if (call.callback)
                    call.callback(
                        {}, QStringLiteral("Daemon disconnected. Retry the credential operation "
                                           "after reconnecting."));
                continue;
            }
            enqueue(std::move(call));
        }
        if (wasReady) {
            emit disconnected(QStringLiteral("KetPlus Daemon disconnected."));
        }
        scheduleReconnect();
    });
    connect(&socket_, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError error) {
                if (error == QLocalSocket::ServerNotFoundError ||
                    error == QLocalSocket::ConnectionRefusedError) {
                    launchDaemon();
                }
                scheduleReconnect();
            });
    QTimer::singleShot(0, this, &DaemonClient::connectToDaemon);
}

DaemonClient::~DaemonClient() {
    reconnectTimer_.stop();
    stableTimer_.stop();
    socket_.disconnectFromServer();
    pending_.clear();
    queued_.clear();
}

bool DaemonClient::isAvailable() const noexcept { return !daemonExecutable().isEmpty(); }

bool DaemonClient::isReady() const noexcept { return ready_; }

void DaemonClient::call(const QString& type, const QJsonObject& payload, const QString& sessionId,
                        const qint64 generation, ResponseCallback callback) {
    QueuedCall call{{}, type, payload, sessionId, generation, std::move(callback), ++nextSequence_};
    enqueue(std::move(call));
    if (!ready_) {
        // A reconnect already scheduled keeps its backoff; connecting now restarts the loop.
        if (!reconnectTimer_.isActive()) {
            connectToDaemon();
        }
        return;
    }
    sendQueued();
}

void DaemonClient::enqueue(QueuedCall call) {
    if (mergesWhileQueued(call.type)) {
        for (QueuedCall& waiting : queued_) {
            if (waiting.type == call.type && waiting.sessionId == call.sessionId &&
                waiting.generation == call.generation && waiting.payload == call.payload) {
                waiting.callback = [first = std::move(waiting.callback),
                                    second = std::move(call.callback)](const QJsonObject& result,
                                                                       const QString& error) {
                    if (first) {
                        first(result, error);
                    }
                    if (second) {
                        second(result, error);
                    }
                };
                return;
            }
        }
    }
    queued_.append(std::move(call));
}

void DaemonClient::sendQueued() {
    while (ready_ && pending_.size() < maximumCallsInFlight && !queued_.isEmpty()) {
        sendCall(queued_.takeFirst());
    }
}

QString DaemonClient::defaultSocketPath() { return DaemonPaths::socket(); }
QString DaemonClient::defaultStatePath() { return DaemonPaths::state(); }
QString DaemonClient::defaultTokenPath() { return DaemonPaths::token(); }

void DaemonClient::connectToDaemon() {
    if (ready_ || socket_.state() == QLocalSocket::ConnectedState ||
        socket_.state() == QLocalSocket::ConnectingState) {
        return;
    }
    socket_.abort();
    socket_.connectToServer(socketPath_);
}

void DaemonClient::scheduleReconnect() {
    if (reconnectTimer_.isActive() || ready_) {
        return;
    }
    const int exponent = qMin(reconnectAttempt_++, 6);
    const int base = qMin(100 * (1 << exponent), 5000);
    const int jitter = QRandomGenerator::global()->bounded(qMax(1, base / 3));
    reconnectTimer_.start(base + jitter);
}

QString DaemonClient::daemonLogPath() {
    const QString directory = QDir(DaemonPaths::dataDirectory()).filePath(QStringLiteral("logs"));
    if (!QDir().mkpath(directory))
        return {};
    const QString path = QDir(directory).filePath(QStringLiteral("daemon.log"));
    // One previous log is kept; a daemon that runs for weeks must not fill the disk.
    constexpr qint64 maximumLogBytes = 8 * 1024 * 1024;
    if (QFileInfo(path).size() > maximumLogBytes) {
        const QString previous = path + QStringLiteral(".1");
        QFile::remove(previous);
        QFile::rename(path, previous);
    }
    QFile file(path);
    if (!file.open(QIODevice::Append))
        return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return path;
}

void DaemonClient::launchDaemon() {
    if (launched_ || qEnvironmentVariableIsSet("KETPLUS_DISABLE_DAEMON_AUTOSTART")) {
        return;
    }
    const QString executable = daemonExecutable();
    if (executable.isEmpty()) {
        return;
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--socket"), socketPath_, QStringLiteral("--state"),
                          statePath_, QStringLiteral("--token-file"), tokenPath_});
    // The daemon's own log lines (build, relay, command lifecycle; never conversation text)
    // are the only record of what it did while the phone was waiting.
    const QString log = daemonLogPath();
    process.setStandardOutputFile(log.isEmpty() ? QProcess::nullDevice() : log, QIODevice::Append);
    process.setStandardErrorFile(log.isEmpty() ? QProcess::nullDevice() : log, QIODevice::Append);
    launched_ = process.startDetached();
}

void DaemonClient::authenticate() {
    const QByteArray token = readLocalDaemonCredential(tokenPath_);
    if (token.isEmpty()) {
        socket_.abort();
        scheduleReconnect();
        return;
    }
    QueuedCall hello{
        {},
        QStringLiteral("daemon.hello"),
        {{QStringLiteral("token"), QString::fromUtf8(token)},
         {QStringLiteral("client_id"), clientId_},
         {QStringLiteral("protocol_minor"), supportedProtocolMinor},
         {QStringLiteral("transcript_activity_groups"), activityGroupsEnabled_}},
        {},
        -1,
        [this](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty() || !result.value(QStringLiteral("ok")).toBool()) {
                socket_.abort();
                scheduleReconnect();
                return;
            }
            stableTimer_.start();
            protocolMinor_ = std::min(supportedProtocolMinor,
                                      result.value(QStringLiteral("protocol_minor")).toInt(0));
            capabilities_.clear();
            for (const QJsonValue& value : result.value(QStringLiteral("capabilities")).toArray())
                if (value.isString())
                    capabilities_.append(value.toString());
            features_.clear();
            for (const QJsonValue& value : result.value(QStringLiteral("features")).toArray())
                features_.append(value.toString());
            const QString daemonVersion = result.value(QStringLiteral("daemon_version")).toString();
            const QVersionNumber running = QVersionNumber::fromString(daemonVersion);
            const QVersionNumber bundled =
                QVersionNumber::fromString(QStringLiteral(KETPLUS_PRODUCT_VERSION));
            if (KETPLUS_AUTO_UPGRADE_DAEMON && !replacementAttempted_ && !running.isNull() &&
                !bundled.isNull() && QVersionNumber::compare(running, bundled) < 0) {
                replaceIdleDaemon(daemonVersion);
                return;
            }
            finishAuthentication();
        }};
    sendCall(std::move(hello));
}

void DaemonClient::finishAuthentication() {
    setReady(true);
    sendQueued();
}

void DaemonClient::replaceIdleDaemon(const QString& daemonVersion) {
    replacementAttempted_ = true;
    QueuedCall request{
        {},
        QStringLiteral("daemon.prepare_upgrade"),
        {{QStringLiteral("current_version"), daemonVersion},
         {QStringLiteral("replacement_version"), QStringLiteral(KETPLUS_PRODUCT_VERSION)}},
        {},
        -1,
        [this](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty() || !result.value(QStringLiteral("ok")).toBool()) {
                // Never terminate a daemon that owns an active turn. The exact
                // protocol-version check performed during hello makes it safe for
                // the new Desktop to keep using the previous helper until idle.
                finishAuthentication();
            }
        }};
    sendCall(std::move(request));
}

void DaemonClient::sendCall(QueuedCall call) {
    if (call.messageId.isEmpty()) {
        call.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const QString messageId = call.messageId;
    QJsonObject envelope{
        {QStringLiteral("protocol_version"), protocolVersion},
        {QStringLiteral("message_id"), messageId},
        {QStringLiteral("message_type"), call.type},
        {QStringLiteral("sent_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("payload"), call.payload}};
    if (!call.sessionId.isEmpty()) {
        envelope.insert(QStringLiteral("session_id"), call.sessionId);
    }
    if (call.generation >= 0) {
        envelope.insert(QStringLiteral("session_generation"), call.generation);
    }
    pending_.insert(messageId, std::move(call));
    sendEnvelope(envelope);
}

void DaemonClient::sendEnvelope(const QJsonObject& envelope) {
    const QByteArray frame = encodeFrame(envelope);
    if (frame.size() > maximumFrameBytes + 4 || socket_.state() != QLocalSocket::ConnectedState) {
        socket_.abort();
        return;
    }
    socket_.write(frame);
}

void DaemonClient::readFrames() {
    buffer_.append(socket_.readAll());
    while (true) {
        if (expectedSize_ == 0) {
            if (buffer_.size() < 4) {
                return;
            }
            expectedSize_ = (static_cast<quint32>(static_cast<uchar>(buffer_[0])) << 24U) |
                            (static_cast<quint32>(static_cast<uchar>(buffer_[1])) << 16U) |
                            (static_cast<quint32>(static_cast<uchar>(buffer_[2])) << 8U) |
                            static_cast<quint32>(static_cast<uchar>(buffer_[3]));
            buffer_.remove(0, 4);
            if (expectedSize_ == 0 || expectedSize_ > maximumFrameBytes) {
                socket_.abort();
                return;
            }
        }
        if (buffer_.size() < static_cast<qsizetype>(expectedSize_)) {
            return;
        }
        const QByteArray frame = buffer_.left(static_cast<qsizetype>(expectedSize_));
        buffer_.remove(0, static_cast<qsizetype>(expectedSize_));
        expectedSize_ = 0;
        consumeFrame(frame);
    }
}

void DaemonClient::consumeFrame(const QByteArray& frame) {
    const QJsonDocument document = QJsonDocument::fromJson(frame);
    if (!document.isObject()) {
        socket_.abort();
        return;
    }
    const QJsonObject envelope = document.object();
    if (envelope.value(QStringLiteral("message_type")).toString() ==
        QStringLiteral("session.event")) {
        emit eventReceived(envelope);
        return;
    }
    if (const QString type = envelope.value(QStringLiteral("message_type")).toString();
        type.startsWith(QStringLiteral("team.")) &&
        !envelope.contains(QStringLiteral("correlation_id"))) {
        emit teamEventReceived(type, envelope.value(QStringLiteral("payload")).toObject());
        return;
    }
    if (envelope.value(QStringLiteral("message_type")).toString() ==
        QStringLiteral("tags.changed")) {
        emit noticeReceived(QStringLiteral("tags.changed"),
                            envelope.value(QStringLiteral("payload")).toObject());
        return;
    }
    const QString correlationId = envelope.value(QStringLiteral("correlation_id")).toString();
    const QueuedCall call = pending_.take(correlationId);
    sendQueued();
    if (!call.callback) {
        return;
    }
    const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    if (!payload.value(QStringLiteral("ok")).toBool()) {
        call.callback(payload,
                      payload.value(QStringLiteral("error_message"))
                          .toString(QStringLiteral("KetPlus Daemon rejected the request.")));
        return;
    }
    call.callback(payload, {});
}

void DaemonClient::setReady(const bool ready) {
    if (!ready)
        capabilities_.clear();
    if (ready_ == ready) {
        return;
    }
    ready_ = ready;
    emit readyChanged(ready_);
}

void DaemonClient::failPending(const QString& reason) {
    const auto callsInFlight = std::exchange(pending_, {});
    for (const QueuedCall& call : callsInFlight) {
        if (call.callback) {
            call.callback({}, reason);
        }
    }
    const auto calls = std::exchange(queued_, {});
    for (const QueuedCall& call : calls) {
        if (call.callback) {
            call.callback({}, reason);
        }
    }
}

QString DaemonClient::daemonExecutable() {
    const QString configured = qEnvironmentVariable("KETPLUS_DAEMON_EXECUTABLE").trimmed();
    if (!configured.isEmpty() && QFileInfo(configured).isExecutable()) {
        return QFileInfo(configured).absoluteFilePath();
    }
    const QDir applicationDirectory(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        applicationDirectory.filePath(QStringLiteral("ketplus_daemon")),
        applicationDirectory.filePath(QStringLiteral("../Helpers/ketplus_daemon")),
        applicationDirectory.filePath(QStringLiteral("../../../ketplus_daemon")),
        QStandardPaths::findExecutable(QStringLiteral("ketplus_daemon"))};
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

QByteArray DaemonClient::encodeFrame(const QJsonObject& envelope) {
    const QByteArray payload = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    QByteArray frame(4, Qt::Uninitialized);
    const quint32 size = static_cast<quint32>(payload.size());
    frame[0] = static_cast<char>((size >> 24U) & 0xffU);
    frame[1] = static_cast<char>((size >> 16U) & 0xffU);
    frame[2] = static_cast<char>((size >> 8U) & 0xffU);
    frame[3] = static_cast<char>(size & 0xffU);
    frame.append(payload);
    return frame;
}

} // namespace ketplus::remote
