#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QLockFile;
class QLocalServer;
class QLocalSocket;

namespace ketplus::daemon {

class ClaudeCredentials;
class SessionHost;
class CloudControlClient;

class LocalIpcServer final : public QObject {
    Q_OBJECT

  public:
    LocalIpcServer(SessionHost* host, const QString& serverName, const QByteArray& token,
                   CloudControlClient* cloud = nullptr, QObject* parent = nullptr);
    ~LocalIpcServer() override;

    [[nodiscard]] bool listen();
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] QString serverName() const;
    // The absolute endpoint a child process can connect to, independent of its environment.
    [[nodiscard]] QString fullServerName() const;
    void close();
    void setClaudeCredentials(ClaudeCredentials* credentials) { credentials_ = credentials; }

    static constexpr int defaultHelloTimeoutMs = 10'000;
    static constexpr int defaultMaximumConnections = 64;
    // Connections that have not completed daemon.hello within this interval are closed.
    void setHelloTimeout(int milliseconds);
    // Connections beyond this count receive a connection_limit failure and are closed.
    void setMaximumConnections(int count);
    [[nodiscard]] int connectionCount() const;

  signals:
    void shutdownRequested();

  private:
    struct Connection final {
        QByteArray frame;
        quint32 expectedSize{0};
        QString clientId;
        bool authenticated{false};
        bool activityGroups{false};
        // ADR 0027: a ketplus_agents MCP server, authenticated by a conversation token. It may
        // only call agents.* operations and never receives session events.
        QString agentToken;
    };

    void acceptConnections();
    void readClient(QLocalSocket* socket);
    void consumeFrame(QLocalSocket* socket, const QByteArray& frame);
    void send(QLocalSocket* socket, const QJsonObject& envelope);
    void broadcastEvent(const QJsonObject& event);
    void broadcastTeamEvent(const QString& type, const QJsonObject& payload);
    void broadcastNotice(const QString& type, const QJsonObject& payload);
    [[nodiscard]] bool tokenMatches(const QByteArray& candidate) const;

    ClaudeCredentials* credentials_{nullptr};
    SessionHost* host_{nullptr};
    CloudControlClient* cloud_{nullptr};
    QLocalServer* server_{nullptr};
    std::unique_ptr<QLockFile> lock_;
    QString serverName_;
    QString error_;
    QByteArray token_;
    bool shuttingDown_{false};
    int helloTimeoutMs_{defaultHelloTimeoutMs};
    int maximumConnections_{defaultMaximumConnections};
    QHash<QLocalSocket*, Connection> connections_;
};

} // namespace ketplus::daemon
