#include "cloud-privacy/CloudPrivacyTestAccess.h"
#include "cloud-privacy/FakeTransport.h"
#include "cloud-privacy/MemoryCredentials.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "workspace/ReadOnlyWorkspaceFiles.h"
#include <QDir>
#include <QMessageAuthenticationCode>
#include <QProcess>

#include <QFile>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;
using Access = CloudPrivacyTestAccess;

namespace {
// An actual HTTP test relay on a fresh loopback port. It owns no persistent state,
// accepts only the synthetic ACK route and never forwards requests elsewhere.
class LoopbackRelay final : public QTcpServer {
  public:
    QByteArray body;
    QByteArray senderKeys;
    bool acceptHeartbeat{false};
    LoopbackRelay() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (buffer->size() > 1024 * 1024)
                        qFatal("Test relay request exceeds bound");
                    const auto headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    if (buffer->startsWith("GET /v2/device-keys/phone/1 HTTP/1.1\r\n") &&
                        !senderKeys.isEmpty()) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                      "application/json\r\nContent-Length: " +
                                      QByteArray::number(senderKeys.size()) +
                                      "\r\nConnection: close\r\n\r\n" + senderKeys);
                        socket->disconnectFromHost();
                        return;
                    }
                    if (buffer->startsWith("POST /v2/daemon/heartbeat HTTP/1.1\r\n")) {
                        socket->write(acceptHeartbeat
                                          ? "HTTP/1.1 202 Accepted\r\nContent-Length: "
                                            "2\r\nConnection: close\r\n\r\n{}"
                                          : "HTTP/1.1 503 Service Unavailable\r\nContent-Length: "
                                            "2\r\nConnection: close\r\n\r\n{}");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (!buffer->startsWith("POST /v2/commands/context-files/ack HTTP/1.1\r\n"))
                        qFatal("Unexpected test relay route: %s",
                               buffer->split('\n').first().constData());
                    qint64 length = -1;
                    for (const auto& line : buffer->left(headerEnd).toLower().split('\n')) {
                        if (line.startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    }
                    if (length < 0 || length > 1024 * 1024)
                        qFatal("Test relay requires a bounded Content-Length");
                    if (buffer->size() - headerEnd - 4 < length)
                        return;
                    body = buffer->mid(headerEnd + 4, length);
                    // Prevent background event-upload scheduling after the captured ACK.
                    socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 2\r\n"
                                  "Connection: close\r\n\r\n{}");
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
};

class LoopbackOnlyTransport final : public QNetworkAccessManager {
  public:
    explicit LoopbackOnlyTransport(quint16 port) : port_(port) { setProxy(QNetworkProxy::NoProxy); }

  protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request,
                                 QIODevice* outgoing) override {
        if (request.url().scheme() != "http" || request.url().host() != "127.0.0.1" ||
            request.url().port() != port_)
            qFatal("Test transport attempted to leave its private loopback relay");
        return QNetworkAccessManager::createRequest(operation, request, outgoing);
    }

  private:
    quint16 port_;
};
} // namespace

// Uses the production daemon's encryption and request serialization. Both relay
// variants are isolated; MemoryCredentials replaces Keychain in this executable.
class WorktreeContextRelayTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { r03::resetCredentials(); }

    void encryptedWorktreeCommandsUsePrivateRelay_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("foreignRepository");
        QTest::addColumn<QString>("adapter");
        QTest::newRow("paged-catalog") << "session.worktrees" << false << "codex";
        QTest::newRow("switch-context") << "session.worktree.switch" << false << "codex";
        QTest::newRow("switch-another-repository") << "session.worktree.switch" << true << "codex";
        QTest::newRow("effective-settings-without-provider")
            << "session.configuration.read" << false << "codex";
        QTest::newRow("claude-paged-catalog") << "session.worktrees" << false << "claude";
        QTest::newRow("claude-switch-context") << "session.worktree.switch" << false << "claude";
        QTest::newRow("claude-switch-another-repository")
            << "session.worktree.switch" << true << "claude";
    }
    void encryptedWorktreeCommandsUsePrivateRelay() {
        QFETCH(QString, kind);
        QFETCH(bool, foreignRepository);
        QFETCH(QString, adapter);
        QTemporaryDir dir;
        const auto a = dir.filePath("repo");
        const auto b = dir.filePath("feature");
        QVERIFY(QDir().mkpath(a));
        QCOMPARE(QProcess::execute("git", {"-C", a, "init", "-q"}), 0);
        QCOMPARE(QProcess::execute("git", {"-C", a, "-c", "user.name=Fixture", "-c",
                                           "user.email=fixture@example.invalid", "-c",
                                           "commit.gpgsign=false", "commit", "--allow-empty", "-qm",
                                           "initial"}),
                 0);
        if (foreignRepository) {
            QVERIFY(QDir().mkpath(b));
            QCOMPARE(QProcess::execute("git", {"-C", b, "init", "-q"}), 0);
        } else {
            QCOMPARE(QProcess::execute("git", {"-C", a, "worktree", "add", "-b", "feature", b}), 0);
        }
        LocalJournal journal(dir.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "synthetic-conversation";
        session.worktreePath = a;
        session.adapter = adapter;
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        QVERIFY(journal.rememberWorktree(QFileInfo(b).canonicalFilePath()));
        CodexAdapter provider(dir.filePath("never-start-provider"));
        SessionHost host(&journal, &provider);
        LoopbackRelay server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        LoopbackOnlyTransport network(server.serverPort());
        CloudControlClient client(&journal, &host);
        Access::configure(client, network,
                          QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort())));
        Access::machineKeyVersion(client, 1);
        const QByteArray salt(32, 's');
        Access::pathSalt(client, salt);
        server.acceptHeartbeat = kind == "session.worktrees";
        Access::pinDevice(client, "phone", 1, Access::crypto(client).publicKeys());
        server.senderKeys =
            QJsonDocument(QJsonObject{{"keys", Access::crypto(client).publicKeys()}})
                .toJson(QJsonDocument::Compact);
        auto id = [&](const QByteArray& label, const QString& path) {
            return QString::fromLatin1(
                QMessageAuthenticationCode::hash(label + '\n' +
                                                     QFileInfo(path).canonicalFilePath().toUtf8(),
                                                 salt, QCryptographicHash::Sha256)
                    .toHex());
        };
        QJsonObject command{{"id", "context-files"},
                            {"sessionId", session.id},
                            {"sessionGeneration", 1},
                            {"kind", kind},
                            {"attempt", 1},
                            {"claimToken", "synthetic-claim"},
                            {"workspaceId", "workspace-synthetic"},
                            {"machineId", "machine-synthetic"},
                            {"repositoryId", id("repository", foreignRepository ? b : a)},
                            {"worktreeId", id("worktree", b)}};
        QJsonObject payload{{"expected_context_revision", 1}, {"limit", 1}, {"repository_path", b}};
        if (kind == "session.configuration.read")
            payload.insert("effective_only", true);
        command.insert("content",
                       Access::crypto(client).encrypt("{}", "seed", session.id, 1, 1, "phone", 1));
        command.insert("content",
                       Access::crypto(client).encrypt(
                           QJsonDocument(payload).toJson(QJsonDocument::Compact),
                           Access::commandAad(client, command), session.id, 1, 1, "phone", 1));
        Access::validCredential(client);
        QObject::disconnect(&host, nullptr, &client,
                            nullptr); // This fixture tests command ACKs, not event replication.
        Access::execute(client, command);
        QTRY_VERIFY_WITH_TIMEOUT(!server.body.isEmpty(), 5000);
        const auto ack = QJsonDocument::fromJson(server.body).object();
        QCOMPARE(ack.value("resultCode").toString(), "ok");
        QVERIFY(!ack.value("encryptedResult").toObject().isEmpty());
        QVERIFY(!server.body.contains(a.toUtf8()));
        QVERIFY(!server.body.contains(b.toUtf8()));
        QCOMPARE(journal.sessions().first().contextRevision, kind.endsWith("switch") ? 2 : 1);
        QCOMPARE(QFileInfo(journal.sessions().first().worktreePath).canonicalFilePath(),
                 QFileInfo(kind.endsWith("switch") ? b : a).canonicalFilePath());
    }

    void explicitHistoricalContextReadsOriginalRoot_data() {
        QTest::addColumn<int>("context");
        QTest::addColumn<QString>("expected");
        QTest::newRow("historical-a") << 1 << "ok";
        QTest::newRow("wrong-context-b") << 2 << "scope_forbidden";
        QTest::newRow("unknown-context") << 99 << "resource_context_unknown";
        QTest::newRow("event-provenance") << 3 << "ok";
        QTest::newRow("forged-event-provenance") << 4 << "resource_context_unknown";
    }

    void explicitHistoricalContextReadsOriginalRoot() {
        QFETCH(int, context);
        QFETCH(QString, expected);
        QTemporaryDir directory;
        const auto a = directory.filePath("a");
        const auto b = directory.filePath("b");
        QVERIFY(QDir().mkpath(a));
        QVERIFY(QDir().mkpath(b));
        QCOMPARE(QProcess::execute("git", {"-C", a, "init", "-q"}), 0);
        QCOMPARE(QProcess::execute("git", {"-C", a, "-c", "user.name=Fixture", "-c",
                                           "user.email=fixture@example.invalid", "-c",
                                           "commit.gpgsign=false", "commit", "--allow-empty", "-qm",
                                           "initial"}),
                 0);
        QFile file(QDir(a).filePath("source.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original A");
        file.close();
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "synthetic-conversation";
        session.worktreePath = a;
        session.adapter = "codex";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        QVERIFY(journal.switchSessionWorktree(session, b, "switch", {{"ok", true}}) > 0);
        CodexAdapter provider(directory.filePath("unused-provider"));
        SessionHost host(&journal, &provider);
        LoopbackRelay server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        LoopbackOnlyTransport network(server.serverPort());
        CloudControlClient client(&journal, &host);
        Access::configure(client, network,
                          QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
        Access::machineKeyVersion(client, 1);
        const QByteArray salt(32, 's');
        Access::pathSalt(client, salt);
        auto id = [&](const QByteArray& kind) {
            return QString::fromLatin1(QMessageAuthenticationCode::hash(
                                           kind + '\n' + QFileInfo(a).canonicalFilePath().toUtf8(),
                                           salt, QCryptographicHash::Sha256)
                                           .toHex());
        };
        const WorkspaceFileScope scope{"owner",          "workspace-synthetic", "machine-synthetic",
                                       id("repository"), id("worktree"),        a};
        const QJsonObject command{{"id", "context-files"},
                                  {"sessionId", session.id},
                                  {"sessionGeneration", 1},
                                  {"kind", "workspace.files.file"},
                                  {"attempt", 1},
                                  {"claimToken", "synthetic-claim"},
                                  {"ownerSubjectId", "owner"},
                                  {"workspaceId", scope.workspaceId},
                                  {"machineId", scope.machineId},
                                  {"repositoryId", scope.repositoryId},
                                  {"worktreeId", scope.worktreeId},
                                  {"workspaceRevision", ReadOnlyWorkspaceFiles(scope).revision()}};
        QJsonObject payload{{"path", "source.txt"}, {"resource_context_revision", context}};
        if (context == 3 || context == 4) {
            const auto cursor = journal.appendEvent(
                session.id, 1, "item.completed",
                {{"context_revision", context},
                 {"item",
                  QJsonObject{{"type", "fileChange"},
                              {"changes", QJsonArray{QJsonObject{{"path", file.fileName()}}}}}}});
            payload.insert("resource_event_cursor", cursor);
            payload.insert("resource_source_path",
                           context == 3 ? file.fileName() : a + "/secret.txt");
        }
        Access::workspaceFiles(client, command, payload);
        QTRY_VERIFY_WITH_TIMEOUT(!server.body.isEmpty(), 3000);
        const auto ack = QJsonDocument::fromJson(server.body).object();
        QCOMPARE(ack.value("resultCode").toString(), expected);
        QVERIFY(!ack.value("encryptedResult").toObject().isEmpty());
        QVERIFY(!server.body.contains("original A"));
    }

    void switchedConversationRejectsLegacyFilesBeforeResolvingTheirPath_data() {
        QTest::addColumn<bool>("loopback");
        QTest::newRow("in-memory-relay") << false;
        QTest::newRow("separate-loopback-relay") << true;
    }

    void switchedConversationRejectsLegacyFilesBeforeResolvingTheirPath() {
        QFETCH(bool, loopback);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = "synthetic-conversation";
        session.worktreePath = directory.path();
        session.adapter = "codex";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        QVERIFY(journal.switchSessionWorktree(session, directory.path(), "isolated-switch",
                                              {{"ok", true}}) > 0);
        CodexAdapter provider(directory.filePath("nonexistent-provider"));
        SessionHost host(&journal, &provider);
        r03::Transport relay;
        // Rejecting the ACK at the fixture stops background upload scheduling; the
        // assertion concerns the ACK body produced by the real daemon boundary.
        relay.status = 503;
        LoopbackRelay server;
        if (loopback)
            QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        LoopbackOnlyTransport network(server.serverPort());
        CloudControlClient client(&journal, &host);
        if (loopback)
            Access::configure(client, network,
                              QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
        else
            Access::configure(client, relay);
        Access::machineKeyVersion(client, 1);
        const QJsonObject command{{"id", "context-files"},  {"sessionId", session.id},
                                  {"sessionGeneration", 1}, {"kind", "workspace.files.file"},
                                  {"attempt", 1},           {"claimToken", "synthetic-claim"}};
        Access::workspaceFiles(client, command, {{"path", "same-relative-path.txt"}});
        QByteArray body;
        QByteArray senderKeys;
        bool acceptHeartbeat{false};
        if (loopback) {
            QTRY_VERIFY_WITH_TIMEOUT(!server.body.isEmpty(), 2000);
            body = server.body;
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(relay.requests.size(), 1, 2000);
            QCOMPARE(relay.requests.first().url().host(), "synthetic.invalid");
            QCOMPARE(relay.requests.first().url().path(), "/v2/commands/context-files/ack");
            body = relay.bodies.first();
        }
        const auto ack = QJsonDocument::fromJson(body).object();
        QCOMPARE(ack.value("status").toString(), "rejected");
        QCOMPARE(ack.value("resultCode").toString(), "context_upgrade_required");
        QVERIFY(!ack.value("encryptedResult").toObject().isEmpty());
        QVERIFY(!body.contains("same-relative-path.txt"));
        QCOMPARE(journal.sessions().first().contextRevision, 2);
        QCOMPARE(journal.sessions().first().generation, 1);
        QVERIFY(journal.sessions().first().threadId.isEmpty());
    }
};

QTEST_GUILESS_MAIN(WorktreeContextRelayTest)
#include "WorktreeContextRelayTest.moc"
