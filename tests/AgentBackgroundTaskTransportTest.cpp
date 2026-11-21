#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

// A protocol peer only: it cannot launch a daemon, command or provider.
class TaskPeer final : public QObject {
  public:
    QTemporaryDir directory{QDir::tempPath() + "/t4-XXXXXX"};
    QLocalServer server;
    QPointer<QLocalSocket> client;
    QByteArray buffer;
    QList<QJsonObject> requests;
    std::function<QJsonObject(const QJsonObject&)> respond;
    bool control{true};
    QString provider{"codex"};

    TaskPeer() {
        const auto path = directory.filePath("peer.sock");
        const auto token = directory.filePath("peer.token");
        QFile file(token);
        if (!file.open(QIODevice::WriteOnly) ||
            !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(QByteArray(40, 't')) != 40 || !server.listen(path))
            qFatal("Unable to create isolated task peer");
        file.close();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DAEMON_SOCKET", path.toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        connect(&server, &QLocalServer::newConnection, this, [this] {
            client = server.nextPendingConnection();
            connect(client, &QLocalSocket::readyRead, this, [this] { read(); });
        });
    }
    ~TaskPeer() override {
        qunsetenv("KETPLUS_DAEMON_SOCKET");
        qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
    }
    void answer(const QJsonObject& request, const QJsonObject& payload) {
        const auto bytes =
            QJsonDocument(QJsonObject{{"correlation_id", request.value("message_id")},
                                      {"message_type", request.value("message_type")},
                                      {"payload", payload}})
                .toJson(QJsonDocument::Compact);
        QByteArray frame(4, Qt::Uninitialized);
        const auto size = quint32(bytes.size());
        for (int i = 0; i < 4; ++i)
            frame[i] = char(size >> (24 - 8 * i));
        client->write(frame + bytes);
        client->flush();
    }
    void read() {
        buffer.append(client->readAll());
        while (buffer.size() >= 4) {
            quint32 size = 0;
            for (int i = 0; i < 4; ++i)
                size = (size << 8) | uchar(buffer[i]);
            if (buffer.size() < size + 4)
                return;
            const auto request = QJsonDocument::fromJson(buffer.mid(4, size)).object();
            buffer.remove(0, size + 4);
            if (request.value("message_type") == "daemon.hello") {
                QJsonArray capabilities{"session.read"};
                if (control)
                    capabilities.append("session.control");
                answer(request,
                       {{"ok", true}, {"protocol_minor", 1}, {"capabilities", capabilities}});
            } else {
                requests.append(request);
                const auto reply = respond ? respond(request) : QJsonObject{{"ok", true}};
                if (!reply.isEmpty())
                    answer(request, reply);
            }
        }
    }
    QJsonObject catalog() const {
        return {{"ok", true},
                {"sessions", QJsonArray{QJsonObject{{"session_id", "owner"},
                                                    {"session_generation", 1},
                                                    {"worktree_path", directory.path()},
                                                    {"adapter", provider},
                                                    {"state", "ready"}}}}};
    }
};

class AgentBackgroundTaskTransportTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void completeRegistryUsesOpaquePagesAndRefetchesConflicts_data() {
        QTest::addColumn<QString>("provider");
        for (const auto& provider : {"codex", "claude", "cursor", "copilot"})
            QTest::newRow(provider) << QString(provider);
    }
    void completeRegistryUsesOpaquePagesAndRefetchesConflicts() {
        QFETCH(QString, provider);
        TaskPeer peer;
        peer.provider = provider;
        bool stale = false;
        int firstPages = 0;
        bool ownerCorrect = true;
        bool cursorCorrect = true;
        peer.respond = [&](const QJsonObject& request) {
            const auto type = request.value("message_type").toString();
            if (type == "session.list")
                return peer.catalog();
            if (type == "session.task.get") {
                const auto id = request.value("payload").toObject().value("task_id").toString();
                return QJsonObject{
                    {"ok", true},
                    {"task",
                     QJsonObject{{"task_id", id},
                                 {"schema_version", 1},
                                 {"state", id == "job-23" ? "completed" : "needs_attention"},
                                 {"revision", 4}}}};
            }
            if (type != "session.task.list")
                return QJsonObject{{"ok", true}, {"events", QJsonArray{}}};
            ownerCorrect = ownerCorrect && request.value("session_id").toString() == "owner";
            const auto cursor = request.value("payload").toObject().value("cursor").toString();
            if (!cursor.isEmpty() && stale) {
                stale = false;
                return QJsonObject{{"ok", false},
                                   {"error_code", "stale_cursor"},
                                   {"error_message", "Registry changed"}};
            }
            QJsonArray tasks;
            if (cursor.isEmpty()) {
                ++firstPages;
                for (int i = 0; i < 24; ++i)
                    tasks.append(QJsonObject{{"task_id", QString("job-%1").arg(i)},
                                             {"schema_version", 1},
                                             {"state", "queued"},
                                             {"revision", 3}});
            } else {
                cursorCorrect = cursorCorrect && cursor == "opaque/+page==";
                tasks.append(QJsonObject{{"task_id", "last-job"},
                                         {"schema_version", 1},
                                         {"state", "needs_attention"},
                                         {"revision", 3}});
            }
            return QJsonObject{{"ok", true},
                               {"tasks", tasks},
                               {"revision", 3},
                               {"active_count", 2},
                               {"next_cursor", cursor.isEmpty() ? QJsonValue("opaque/+page==")
                                                                : QJsonValue(QJsonValue::Null)}};
        };
        ketplus::CodexAppServer server;
        ketplus::AgentSessionManager manager(&server);
        QTRY_VERIFY(server.hasDaemonSession("owner"));
        manager.activate(peer.directory.path(), false);
        QTRY_COMPARE(manager.backgroundTasks(peer.directory.path()).size(), 24);
        QTRY_VERIFY(!manager.backgroundTaskState(peer.directory.path()).value("loading").toBool());
        QCOMPARE(manager.backgroundTaskState(peer.directory.path()).value("nextCursor").toString(),
                 QString("opaque/+page=="));
        QVERIFY(manager.backgroundTaskState(peer.directory.path()).value("canControl").toBool());
        QCOMPARE(manager.backgroundTaskState(peer.directory.path()).value("activeCount").toInt(),
                 2);
        stale = true;
        const auto before = firstPages;
        manager.loadBackgroundTasks(peer.directory.path(), true);
        QTRY_VERIFY(firstPages > before);
        QTRY_VERIFY(!manager.backgroundTaskState(peer.directory.path()).value("loading").toBool());
        manager.loadBackgroundTasks(peer.directory.path(), true);
        QTRY_COMPARE(manager.backgroundTasks(peer.directory.path()).size(), 25);
        QVERIFY(manager.backgroundTaskState(peer.directory.path())
                    .value("nextCursor")
                    .toString()
                    .isEmpty());
        QVERIFY(ownerCorrect);
        QVERIFY(cursorCorrect);
        QJsonArray projection;
        for (int i = 0; i < 16; ++i)
            projection.append(QJsonObject{{"task_id", QString("job-%1").arg(i)},
                                          {"schema_version", 1},
                                          {"state", "queued"},
                                          {"revision", 3}});
        server.notificationReceived("thread/backgroundTasks/updated",
                                    {{"threadId", "owner"}, {"tasks", projection}});
        QCOMPARE(manager.backgroundTasks(peer.directory.path()).size(), 25);
        QTRY_VERIFY(([&] {
            const auto rows = manager.backgroundTasks(peer.directory.path());
            return std::any_of(rows.cbegin(), rows.cend(), [](const QVariant& value) {
                return value.toMap().value("task_id") == "job-23" &&
                       value.toMap().value("state") == "completed";
            });
        })());
        QTRY_VERIFY(!manager.backgroundTaskState(peer.directory.path()).value("loading").toBool());
        bool acknowledged = false;
        server.call(
            "task/resolve_cleanup",
            {{"threadId", "owner"}, {"taskId", "last-job"}, {"revision", 3}},
            [&](const QJsonObject&, const QString& error) { acknowledged = error.isEmpty(); });
        QTRY_VERIFY(acknowledged);
        QCOMPARE(peer.requests.last().value("message_type").toString(),
                 "session.task.resolve_cleanup");
        QCOMPARE(peer.requests.last().value("payload").toObject().value("revision").toInt(), 3);
        peer.client->abort();
        QTRY_VERIFY(!server.isReady());
        QString offlineError;
        server.call("task/stop", {{"threadId", "owner"}, {"taskId", "last-job"}},
                    [&](const QJsonObject&, const QString& error) { offlineError = error; });
        QVERIFY(!offlineError.isEmpty());
    }

    void controlPermissionAndPayloadAreProviderIndependent_data() {
        QTest::addColumn<QString>("provider");
        for (const auto& provider : {"codex", "claude", "cursor", "copilot"})
            QTest::newRow(provider) << QString(provider);
    }
    void controlPermissionAndPayloadAreProviderIndependent() {
        QFETCH(QString, provider);
        TaskPeer peer;
        peer.provider = provider;
        peer.control = false;
        peer.respond = [&](const QJsonObject& request) {
            return request.value("message_type") == "session.list"
                       ? peer.catalog()
                       : QJsonObject{{"ok", true}, {"events", QJsonArray{}}};
        };
        ketplus::CodexAppServer server;
        QTRY_VERIFY(server.hasDaemonSession("owner"));
        QVERIFY(server.backgroundTaskAccess().value("canRead").toBool());
        QVERIFY(!server.backgroundTaskAccess().value("canControl").toBool());
        QString denied;
        server.call("task/stop", {{"threadId", "owner"}, {"taskId", "job"}},
                    [&](const QJsonObject&, const QString& error) { denied = error; });
        QVERIFY(!denied.isEmpty());
        for (const auto& request : peer.requests)
            QVERIFY(request.value("message_type") != "session.task.stop");
        bool read = false;
        server.call("task/output",
                    {{"threadId", "owner"}, {"taskId", "job"}, {"cursor", "opaque/output=="}},
                    [&](const QJsonObject&, const QString& error) { read = error.isEmpty(); });
        QTRY_VERIFY(read);
        QCOMPARE(peer.requests.last().value("payload").toObject().value("cursor").toString(),
                 "opaque/output==");
        QCOMPARE(peer.requests.last().value("session_generation").toInteger(), 1);
    }
    // The server outlives the manager here: a late answer to one of the manager's calls must not
    // reach its callback, while the server keeps working for everyone else.
    void destroyedManagerLeavesNoCallbackForTheServer() {
        TaskPeer peer;
        peer.respond = [&](const QJsonObject& request) {
            return request.value("message_type").toString() == "session.list" ? peer.catalog()
                                                                              : QJsonObject{};
        };
        ketplus::CodexAppServer server;
        auto* manager = new ketplus::AgentSessionManager(&server);
        QTRY_VERIFY(server.hasDaemonSession("owner"));
        QSignalSpy errors(manager, &ketplus::AgentSessionManager::errorOccurred);
        manager->loadModels();
        const auto modelsRequestIndex = [&] {
            for (qsizetype i = 0; i < peer.requests.size(); ++i)
                if (peer.requests.at(i).value("message_type") == "provider.models")
                    return i;
            return qsizetype(-1);
        };
        QTRY_VERIFY(modelsRequestIndex() >= 0);
        const QJsonObject modelsRequest = peer.requests.at(modelsRequestIndex());
        const int dropped = ketplus::CallbackGuard::droppedCount();
        delete manager;
        QVERIFY(peer.client);
        peer.answer(modelsRequest, {{"ok", false}, {"error_message", "late"}});
        QTRY_COMPARE(ketplus::CallbackGuard::droppedCount(), dropped + 1);

        // Another owner's call on the same server still completes.
        const auto before = peer.requests.size();
        QString outcome = "pending";
        server.call("model/list", {}, [&](const QJsonObject&, const QString& error) {
            outcome = error.isEmpty() ? QString("answered") : error;
        });
        QTRY_VERIFY(peer.requests.size() > before);
        peer.answer(peer.requests.last(), {{"ok", true}, {"models", QJsonArray{}}});
        QTRY_COMPARE(outcome, QString("answered"));

        // And the connection still recovers by itself: the client's own hello is untouched.
        peer.client->abort();
        QTRY_VERIFY_WITH_TIMEOUT(!server.isReady(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(server.isReady(), 20000);
    }
    void liveManagerStillReceivesFailedCalls() {
        TaskPeer peer;
        peer.respond = [&](const QJsonObject& request) {
            return request.value("message_type").toString() == "session.list"
                       ? peer.catalog()
                       : QJsonObject{{"ok", false}, {"error_message", "refused"}};
        };
        ketplus::CodexAppServer server;
        ketplus::AgentSessionManager manager(&server);
        QTRY_VERIFY(server.hasDaemonSession("owner"));
        QSignalSpy errors(&manager, &ketplus::AgentSessionManager::errorOccurred);
        manager.loadModels();
        QTRY_VERIFY(!errors.isEmpty());
        QCOMPARE(errors.first().first().toString(), QString("refused"));
    }
};

QTEST_MAIN(AgentBackgroundTaskTransportTest)
#include "AgentBackgroundTaskTransportTest.moc"
