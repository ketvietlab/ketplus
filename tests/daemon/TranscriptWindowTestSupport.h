#pragma once

#include "FakeCodex.h"
#include "LargeEventReassembler.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentTranscriptPaging.h"
#include "agent/CodexAppServer.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/McpToolResultMedia.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include "storage/McpMediaStore.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::daemon;
using namespace ketplus::agent_message;

namespace {
struct Environment final {
    QTemporaryDir directory{QDir::tempPath() + QStringLiteral("/kptw-XXXXXX")};
    QByteArray socket = qgetenv("KETPLUS_DAEMON_SOCKET");
    QByteArray state = qgetenv("KETPLUS_DAEMON_STATE");
    QByteArray token = qgetenv("KETPLUS_DAEMON_TOKEN_FILE");
    QByteArray executable = qgetenv("KETPLUS_DAEMON_EXECUTABLE");
    Environment() {
        qputenv("KETPLUS_DAEMON_SOCKET", directory.filePath("daemon.sock").toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", directory.filePath("journal.sqlite3").toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", directory.filePath("local.token").toUtf8());
        // The service is hosted in-process below. Still advertise an executable
        // so the production client regards the daemon transport as available
        // before its asynchronous hello completes.
        qputenv("KETPLUS_DAEMON_EXECUTABLE", QCoreApplication::applicationFilePath().toUtf8());
    }
    ~Environment() {
        for (const auto& entry :
             {qMakePair("KETPLUS_DAEMON_SOCKET", socket), qMakePair("KETPLUS_DAEMON_STATE", state),
              qMakePair("KETPLUS_DAEMON_TOKEN_FILE", token),
              qMakePair("KETPLUS_DAEMON_EXECUTABLE", executable)}) {
            if (entry.second.isNull())
                qunsetenv(entry.first);
            else
                qputenv(entry.first, entry.second);
        }
        QSettings().remove(persistedKey(directory.path()));
        QSettings().remove(persistedKey(QFileInfo(directory.path()).canonicalFilePath()));
    }
};

QJsonObject transcriptEvent(const QString& type, const QJsonObject& payload, qint64 cursor,
                            int turn = 1) {
    return {
        {"event_type", type}, {"payload", payload}, {"cursor", cursor}, {"turn_sequence", turn}};
}

// Minimal scripted daemon speaking the local framing protocol, for replies the real
// in-process daemon cannot be made to produce on demand (expired cursors, legacy
// replay, live events for an earlier turn).
class FakeDaemon final : public QObject {
  public:
    using Handler = std::function<QJsonObject(const QString& type, const QJsonObject& envelope)>;

    explicit FakeDaemon(const QString& socketPath, Handler handler) : handler_(std::move(handler)) {
        QLocalServer::removeServer(socketPath);
        listening = server_.listen(socketPath);
        connect(&server_, &QLocalServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                client_ = socket;
                connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
            }
        });
    }

    void push(const QJsonObject& envelope) {
        if (client_)
            write(client_, envelope);
    }

    bool listening{false};
    QStringList received;
    QList<QJsonObject> envelopes;

  private:
    void read(QLocalSocket* socket) {
        buffer_.append(socket->readAll());
        while (buffer_.size() >= 4) {
            const quint32 size = (static_cast<quint32>(static_cast<uchar>(buffer_[0])) << 24U) |
                                 (static_cast<quint32>(static_cast<uchar>(buffer_[1])) << 16U) |
                                 (static_cast<quint32>(static_cast<uchar>(buffer_[2])) << 8U) |
                                 static_cast<quint32>(static_cast<uchar>(buffer_[3]));
            if (buffer_.size() < static_cast<qsizetype>(size) + 4)
                return;
            const QJsonObject envelope =
                QJsonDocument::fromJson(buffer_.mid(4, static_cast<qsizetype>(size))).object();
            buffer_.remove(0, static_cast<qsizetype>(size) + 4);
            const QString type = envelope.value("message_type").toString();
            received.append(type);
            envelopes.append(envelope);
            QJsonObject payload =
                type == "daemon.hello" ? QJsonObject{{"ok", true}} : handler_(type, envelope);
            if (payload.value("__defer_test_response").toBool())
                continue;
            if (!payload.contains("ok"))
                payload.insert("ok", true);
            write(socket, {{"message_type", type + ".result"},
                           {"correlation_id", envelope.value("message_id")},
                           {"payload", payload}});
        }
    }
    static void write(QLocalSocket* socket, const QJsonObject& envelope) {
        const QByteArray body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
        QByteArray frame(4, Qt::Uninitialized);
        const auto size = static_cast<quint32>(body.size());
        frame[0] = static_cast<char>((size >> 24U) & 0xffU);
        frame[1] = static_cast<char>((size >> 16U) & 0xffU);
        frame[2] = static_cast<char>((size >> 8U) & 0xffU);
        frame[3] = static_cast<char>(size & 0xffU);
        socket->write(frame + body);
    }

    Handler handler_;
    QLocalServer server_;
    QLocalSocket* client_{nullptr};
    QByteArray buffer_;
};

QJsonObject liveEvent(const QString& sessionId, const QString& type, const QJsonObject& data,
                      qint64 cursor, qint64 turn) {
    return {{"message_type", "session.event"},
            {"session_id", sessionId},
            {"session_generation", 1},
            {"cursor", cursor},
            {"turn_sequence", turn},
            {"payload", QJsonObject{{"event_type", type}, {"data", data}}}};
}

QJsonObject windowEvent(const QString& sessionId, const QString& type, const QJsonObject& payload,
                        qint64 cursor, qint64 turn) {
    auto event = transcriptEvent(type, payload, cursor, static_cast<int>(turn));
    event.insert("session_id", sessionId);
    event.insert("session_generation", 1);
    return event;
}

void rememberConversation(const QString& directory, const QString& sessionId) {
    const auto canonicalPath = QFileInfo(directory).canonicalFilePath();
    QSettings().setValue(
        persistedKey(canonicalPath),
        QVariantMap{{"path", canonicalPath},
                    {"activeId", sessionId},
                    {"conversations",
                     QVariantList{QVariantMap{
                         {"id", sessionId}, {"threadId", sessionId}, {"title", "Scripted"}}}}});
}

// Owner-only, as the client and daemon require of a credential whatever the umask is.
bool writeToken(const QTemporaryDir& directory) {
    QFile token(directory.filePath("local.token"));
    return token.open(QIODevice::WriteOnly) &&
           token.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
           token.write(QByteArray(48, 'k')) == 48;
}
} // namespace
