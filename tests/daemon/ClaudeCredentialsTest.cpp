#include "providers/ClaudeCredentials.h"
#include "DaemonClient.h"
#include "FakeClaude.h"
#include "ipc/LocalIpcServer.h"
#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAccountUsage.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QLocalSocket>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace ketplus::daemon;
namespace {
struct MemoryStore {
    QByteArray bytes;
    QString error;
    int writes{0};
    ClaudeCredentials::Storage storage() {
        return {[this](QString* e) {
                    *e = error;
                    return bytes;
                },
                [this](const QByteArray& data, QString* e) {
                    *e = error;
                    if (!error.isEmpty())
                        return false;
                    ++writes;
                    bytes = data;
                    return true;
                },
                "test-memory"};
    }
};
class Response : public QNetworkReply {
  public:
    Response(int status, QObject* parent) : QNetworkReply(parent) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit finished();
        });
    }
    void abort() override {}
    qint64 readData(char*, qint64) override { return -1; }
};
class Network : public QNetworkAccessManager {
  public:
    int code{200};
    QNetworkRequest last;
    Operation operation{};
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice*) override {
        last = request;
        operation = op;
        return new Response(code, this);
    }
};
} // namespace

class ClaudeCredentialsTest : public QObject {
    Q_OBJECT
  private slots:
    void adminCredentialsCannotEnterInferenceLaunch() {
        MemoryStore inferenceStore, adminStore;
        ClaudeCredentials credentials(inferenceStore.storage());
        ClaudeAccountUsage analytics(adminStore.storage());
        credentials.setAccountUsage(&analytics);
        QString error;
        const auto reply = [&](const QJsonObject&, const QString& e) { error = e; };
        credentials.handle("save",
                           {{"key", "sk-ant-test-inference"}, {"enabled", true}, {"budget_usd", 5}},
                           reply);
        QVERIFY(error.isEmpty());
        credentials.handle("analytics.save", {{"key", "sk-ant-admin01-test-admin"}}, reply);
        QVERIFY(error.isEmpty());
        const auto launch = credentials.launchConfiguration(&error);
        QCOMPARE(launch.value("key").toString(), "sk-ant-test-inference");
        QVERIFY(!QJsonDocument(launch).toJson().contains("test-admin"));
        credentials.handle(
            "save", {{"key", "sk-ant-admin01-test-admin"}, {"enabled", true}, {"budget_usd", 5}},
            reply);
        QVERIFY(!error.isEmpty());
        credentials.handle("analytics.remove", {}, reply);
        QCOMPARE(credentials.launchConfiguration(&error).value("key").toString(),
                 "sk-ant-test-inference");
        QVERIFY(!adminStore.bytes.contains("test-admin"));
    }
    void desktopClientAcceptsSuccessfulCredentialReplies() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto socketPath = directory.filePath("ipc");
        const auto tokenPath = directory.filePath("local.token");
        const QByteArray token(32, 'x');
        QFile tokenFile(tokenPath);
        QVERIFY(tokenFile.open(QIODevice::WriteOnly));
        QVERIFY(tokenFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(tokenFile.write(token), token.size());
        tokenFile.close();
        const auto oldSocket = qgetenv("KETPLUS_DAEMON_SOCKET");
        const auto oldToken = qgetenv("KETPLUS_DAEMON_TOKEN_FILE");
        const auto restore = qScopeGuard([&] {
            qputenv("KETPLUS_DAEMON_SOCKET", oldSocket);
            qputenv("KETPLUS_DAEMON_TOKEN_FILE", oldToken);
        });
        qputenv("KETPLUS_DAEMON_SOCKET", socketPath.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", tokenPath.toUtf8());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-provider");
        SessionHost host(&journal, &codex);
        MemoryStore memory;
        Network network;
        ClaudeCredentials credentials(memory.storage(), nullptr, &network);
        LocalIpcServer server(&host, socketPath, token);
        server.setClaudeCredentials(&credentials);
        QVERIFY(server.listen());
        ketplus::remote::DaemonClient client;
        const QList<QPair<QString, QJsonObject>> requests{
            {"status", {}},
            {"save", {{"enabled", true}, {"key", "sk-ant-ipc-fixture"}, {"budget_usd", 1}}},
            {"check", {}},
            {"remove", {}},
            {"status", {}}};
        for (const auto& [operation, payload] : requests) {
            bool finished = false;
            QJsonObject response;
            QString error;
            client.call("provider.claude.credentials." + operation, payload, {}, 0,
                        [&](const QJsonObject& value, const QString& message) {
                            response = value;
                            error = message;
                            finished = true;
                        });
            QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
            QVERIFY2(error.isEmpty(), qPrintable(operation + ": " + error));
            QVERIFY(response.value("ok").toBool());
            QVERIFY(!QJsonDocument(response).toJson().contains("sk-ant-"));
            if (operation == "check")
                QVERIFY(response.value("valid").toBool());
            else
                QVERIFY(response.value("supported").toBool());
        }
        bool finished = false;
        QString error;
        client.call("provider.claude.credentials.check", {}, {}, 0,
                    [&](const QJsonObject&, const QString& message) {
                        error = message;
                        finished = true;
                    });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 5000);
        QVERIFY(error.contains("Save an API key"));
    }

    void desktopAndPhoneTurnsUseHostCredentialWithoutJournalingIt() {
        QTemporaryDir directory;
        const auto log = directory.filePath("fake.log");
        const auto executable = directory.filePath("claude");
        QVERIFY(writeFakeClaude(executable, log));
        MemoryStore memory;
        ClaudeCredentials credentials(memory.storage());
        credentials.handle("save",
                           {{"enabled", true}, {"key", "sk-ant-host-fixture"}, {"budget_usd", 2}},
                           [](const auto&, const auto& error) { QVERIFY(error.isEmpty()); });
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-provider");
        ClaudeAdapter claude(executable);
        claude.setCredentials(&credentials);
        SessionHost host(&journal, &codex, &claude);
        for (const QString client : {QString("desktop-test"), QString("paired-phone")}) {
            const QJsonObject request{
                {"message_id", client},
                {"message_type", "session.start_turn"},
                {"payload", QJsonObject{{"worktree_path", directory.path()},
                                        {"prompt", "hello"},
                                        {"options", QJsonObject{{"provider", "claude"},
                                                                {"model", "sonnet"}}}}}};
            const auto started = host.handleCommand(request, client);
            QVERIFY(started["ok"].toBool());
            const auto session = started["session_id"].toString();
            const auto completed = [&] {
                const auto events = journal.eventsAfter(session, 1, 0);
                for (const auto value : events)
                    if (value.toObject()["event_type"] == "turn.completed")
                        return true;
                return false;
            };
            QTRY_VERIFY_WITH_TIMEOUT(completed(), 5000);
            QVERIFY(
                !QJsonDocument(journal.eventsAfter(session, 1, 0)).toJson().contains("sk-ant-"));
        }
        QFile file(log);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto received = file.readAll();
        // Only this test fake records private sidecar input, and only with synthetic keys.
        QCOMPARE(received.count("sk-ant-host-fixture"), 2);
        claude.stop();
    }
    void onlyAuthenticatedLocalIpcCanManageSecrets() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        CodexAdapter codex("/missing-provider");
        SessionHost host(&journal, &codex);
        MemoryStore memory;
        ClaudeCredentials credentials(memory.storage());
        LocalIpcServer server(&host, directory.filePath("ipc"), "test-token");
        server.setClaudeCredentials(&credentials);
        QVERIFY(server.listen());
        QSignalSpy events(&host, &SessionHost::eventReady);
        const auto envelope = [](const QString& type, const QJsonObject& payload) {
            return QJsonObject{{"protocol_version", protocol::currentVersion},
                               {"message_id", type},
                               {"message_type", type},
                               {"sent_at", protocol::timestamp()},
                               {"payload", payload}};
        };
        const QJsonObject secret{
            {"enabled", true}, {"key", "sk-ant-ipc-fixture"}, {"budget_usd", 1}};
        QLocalSocket socket;
        socket.connectToServer(server.serverName());
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        socket.write(protocol::encodeFrame(envelope("provider.claude.credentials.save", secret)));
        QTRY_VERIFY(socket.bytesAvailable() > 4 ||
                    socket.state() == QLocalSocket::UnconnectedState);
        QCOMPARE(memory.writes, 0);
        socket.abort();
        socket.connectToServer(server.serverName());
        QTRY_COMPARE(socket.state(), QLocalSocket::ConnectedState);
        socket.write(protocol::encodeFrame(
            envelope("daemon.hello", {{"token", "test-token"}, {"client_id", "desktop-test"}})));
        QTRY_VERIFY(socket.bytesAvailable() > 4);
        QVERIFY(socket.readAll().contains("\"ok\":true"));
        socket.write(protocol::encodeFrame(envelope("provider.claude.credentials.save", secret)));
        QTRY_VERIFY(socket.bytesAvailable() > 4);
        const auto response = socket.readAll();
        QVERIFY(response.contains("\"configured\":true"));
        QVERIFY(!response.contains("sk-ant-"));
        QCOMPARE(memory.writes, 1);
        QCOMPARE(events.count(), 0);
        // Relay/session commands have no credential route, regardless of their payload.
        const auto remote = host.handleCommand(envelope("provider.claude.credentials.save", secret),
                                               "remote-device");
        QVERIFY(!remote.value("ok").toBool());
        QCOMPARE(memory.writes, 1);
    }
    void saveRotateDisableRemoveAndReload() {
        MemoryStore memory;
        ClaudeCredentials service(memory.storage());
        QJsonObject result;
        QString error;
        const auto reply = [&](const QJsonObject& r, const QString& e) {
            result = r;
            error = e;
        };
        service.handle("status", {}, reply);
        QVERIFY(!result["configured"].toBool());
        service.handle(
            "save", {{"key", "sk-ant-fixture-one"}, {"enabled", true}, {"budget_usd", 2.5}}, reply);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(result["configured"].toBool());
        QVERIFY(!QJsonDocument(result).toJson().contains("sk-ant-"));
        const auto first = service.launchConfiguration(&error);
        QCOMPARE(first["key"].toString(), "sk-ant-fixture-one");
        service.handle("save", {{"enabled", false}, {"budget_usd", 3}}, reply);
        auto disabled = service.launchConfiguration(&error);
        QVERIFY(!disabled["enabled"].toBool());
        QVERIFY(!disabled.contains("key"));
        QVERIFY(memory.bytes.contains("sk-ant-fixture-one"));
        QVERIFY(disabled["revision"] != first["revision"]);
        ClaudeCredentials restarted(memory.storage());
        QCOMPARE(restarted.launchConfiguration(&error), disabled);
        service.handle("save", {{"enabled", true}, {"budget_usd", 3}}, reply);
        QCOMPARE(service.launchConfiguration(&error)["key"], first["key"]);
        service.handle(
            "save", {{"key", "sk-ant-fixture-two"}, {"enabled", true}, {"budget_usd", 0}}, reply);
        QCOMPARE(service.launchConfiguration(&error)["key"].toString(), "sk-ant-fixture-two");
        service.handle("remove", {}, reply);
        QVERIFY(!result["configured"].toBool());
        QVERIFY(!memory.bytes.contains("sk-ant-"));
        QVERIFY(!service.launchConfiguration(&error)["enabled"].toBool());
    }
    void refusesInvalidInputsAndUnreadableStorage() {
        MemoryStore memory;
        ClaudeCredentials service(memory.storage());
        QString error;
        const auto reply = [&](const QJsonObject&, const QString& e) { error = e; };
        for (const QJsonObject payload :
             {QJsonObject{{"enabled", true}, {"budget_usd", 0}},
              QJsonObject{{"key", "sk-ant-bad\n"}, {"enabled", true}, {"budget_usd", 0}},
              QJsonObject{{"key", "sk-ant-fixture"}, {"enabled", true}, {"budget_usd", -1}}}) {
            service.handle("save", payload, reply);
            QVERIFY(!error.isEmpty());
            QCOMPARE(memory.writes, 0);
        }
        memory.bytes = "{broken";
        service.handle("remove", {}, reply);
        QVERIFY(!error.isEmpty());
        QCOMPARE(memory.bytes, QByteArray("{broken"));
        for (const QJsonObject config :
             {QJsonObject{{"enabled", true}, {"revision", "a"}, {"budget_usd", 1}},
              QJsonObject{{"enabled", false}, {"revision", "a"}, {"budget_usd", -1}},
              QJsonObject{{"enabled", false},
                          {"revision", "a"},
                          {"budget_usd", 1},
                          {"key", "sk-ant-bad\n"}}}) {
            memory.bytes = QJsonDocument(config).toJson();
            QVERIFY(service.launchConfiguration(&error).isEmpty());
            QVERIFY(!error.isEmpty());
            service.handle("save", {{"enabled", false}, {"budget_usd", 0}}, reply);
            QVERIFY(!error.isEmpty());
            QCOMPARE(memory.writes, 0);
        }
        memory.error = "Keychain locked";
        QVERIFY(service.launchConfiguration(&error).isEmpty());
        QCOMPARE(error, "Keychain locked");
    }
    void checksOnlyFixedModelsEndpointAndHandlesErrors() {
        MemoryStore memory;
        Network network;
        ClaudeCredentials service(memory.storage(), nullptr, &network);
        service.handle("save", {{"key", "sk-ant-fixture"}, {"enabled", true}, {"budget_usd", 0}},
                       [](const auto&, const auto&) {});
        for (const int code : {200, 401, 403, 429, 302, 500, 0}) {
            network.code = code;
            bool done = false;
            QString error;
            service.handle("check", {{"url", "https://untrusted.invalid"}},
                           [&](const QJsonObject& result, const QString& e) {
                               error = e;
                               done = true;
                               QCOMPARE(result["valid"].toBool(), code == 200);
                           });
            QTRY_VERIFY(done);
            QCOMPARE(error.isEmpty(), code == 200);
            QVERIFY(!error.contains("sk-ant-"));
            QCOMPARE(network.operation, QNetworkAccessManager::GetOperation);
            QCOMPARE(network.last.url().host(), "api.anthropic.com");
            QCOMPARE(network.last.url().path(), "/v1/models");
            QCOMPARE(network.last.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                     int(QNetworkRequest::ManualRedirectPolicy));
        }
    }
    void observationWhitelistNeverReturnsCredentials() {
        MemoryStore memory;
        ClaudeCredentials service(memory.storage());
        service.observe({{"source", "ANTHROPIC_API_KEY"},
                         {"key", "sk-ant-fixture"},
                         {"estimated_cost_usd", 0.15}});
        service.observe({{"source", "sk-ant-fixture"}, {"estimated_cost_usd", -1}});
        service.handle("status", {}, [](const QJsonObject& result, const QString&) {
            const auto observed = result["observation"].toObject();
            QCOMPARE(observed["source"].toString(), "ANTHROPIC_API_KEY");
            QCOMPARE(observed["estimated_cost_usd"].toDouble(), 0.15);
            QVERIFY(!QJsonDocument(result).toJson().contains("sk-ant-"));
        });
    }
};
QTEST_GUILESS_MAIN(ClaudeCredentialsTest)
#include "ClaudeCredentialsTest.moc"
