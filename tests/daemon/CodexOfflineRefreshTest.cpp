#include "agents/AgentToolLaunch.h"
#include "providers/CodexAdapter.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#endif

using namespace ketplus::daemon;
namespace {
QString quoted(const QString& value) {
    const auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}
bool writeFile(const QString& path, const QByteArray& contents, bool executable = false) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
        return false;
    file.close();
    return !executable ||
           QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner);
}
QList<QJsonObject> events(const QString& log) {
    QFile file(log);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> result;
    while (!file.atEnd())
        result.append(QJsonDocument::fromJson(file.readLine()).object());
    return result;
}
bool alive(qint64 pid) {
#ifdef Q_OS_UNIX
    return pid > 0 && (::kill(pid, 0) == 0 || errno != ESRCH);
#else
    Q_UNUSED(pid)
    return false;
#endif
}
bool workersExited(const QString& log) {
    for (const auto& event : events(log))
        if ((event.value("event") == "mcp" || event.value("event") == "provider") &&
            alive(event.value("pid").toInteger()))
            return false;
    return true;
}
struct Cleanup final {
    CodexAdapter& adapter;
    QString log;
    ~Cleanup() {
        adapter.stop();
        QElapsedTimer timer;
        timer.start();
        while (!workersExited(log) && timer.elapsed() < 3000)
            QTest::qWait(20);
        if (!workersExited(log))
            qCritical("Owned offline-test process cleanup was not confirmed.");
    }
};
QJsonObject control(CodexAdapter& adapter, const QString& method, const QJsonObject& params) {
    static const QStringList allowed{
        "thread/start",      "thread/inject_items", "thread/resume",       "thread/read",
        "thread/items/list", "thread/turns/list",   "mcpServerStatus/list"};
    if (!allowed.contains(method))
        return {{"probe_error", "Only no-inference control methods are allowed."}};
    auto state = std::make_shared<QJsonObject>();
    auto done = std::make_shared<bool>(false);
    adapter.call(method, params, [state, done](const QJsonObject& result, const QString& error) {
        *state = error.isEmpty() ? result : QJsonObject{{"probe_error", error}};
        *done = true;
    });
    QElapsedTimer timer;
    timer.start();
    while (!*done && timer.elapsed() < 20000)
        QTest::qWait(10);
    return *done ? *state : QJsonObject{{"probe_error", "Control request timed out."}};
}
bool rolloutContains(const QString& profile, const QString& thread, const QByteArray& marker) {
    QDirIterator files(profile, {"*.jsonl"}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (!QFileInfo(file).canonicalFilePath().startsWith(profile + '/') ||
            !QFileInfo(file).fileName().startsWith("rollout-") || file.size() > 1024 * 1024 ||
            !file.open(QIODevice::ReadOnly))
            continue;
        bool sameThread = false, injectedItem = false;
        while (!file.atEnd()) {
            const auto row = QJsonDocument::fromJson(file.readLine()).object();
            const auto payload = row.value("payload").toObject();
            sameThread |= row.value("type") == "session_meta" && payload.value("id") == thread;
            injectedItem |= row.value("type") == "response_item" &&
                            QJsonDocument(payload).toJson().contains(marker);
        }
        if (sameThread && injectedItem)
            return true;
    }
    return false;
}
QJsonObject inventory(CodexAdapter& adapter, const QString& thread) {
    const auto result =
        control(adapter, "mcpServerStatus/list", {{"threadId", thread}, {"limit", 100}});
    for (const auto& value : result.value("data").toArray())
        if (value.toObject().value("name") == "ketplus_agents")
            return value.toObject().value("tools").toObject();
    return {};
}
} // namespace

class CodexOfflineRefreshTest final : public QObject {
    Q_OBJECT
  private slots:
    void injectedContextSurvivesProductionAdapterRefreshWithoutInference() {
        if (qEnvironmentVariable("KETPLUS_CODEX_OFFLINE_REFRESH") != "1")
            QSKIP("Opt-in installed CLI control test; no model turns are sent.");
#ifndef Q_OS_UNIX
        QSKIP("This offline process-identity fixture requires Unix.");
#endif
        const auto executable = qEnvironmentVariable("KETPLUS_CODEX_TEST_BINARY");
        QVERIFY2(!executable.isEmpty() && QFileInfo(executable).isExecutable(),
                 "Set KETPLUS_CODEX_TEST_BINARY to the already installed CLI.");
        const auto python = QStandardPaths::findExecutable("python3");
        QVERIFY(!python.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto profile = root + "/profile";
        const auto work = root + "/work";
        const auto log = root + "/processes.jsonl";
        QVERIFY(QDir().mkpath(profile));
        QVERIFY(QDir().mkpath(work));

        QTcpServer sentinel;
        QVERIFY(sentinel.listen(QHostAddress::LocalHost));
        int requests = 0;
        connect(&sentinel, &QTcpServer::newConnection, this, [&] {
            while (auto* socket = sentinel.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    if (socket->property("rejected").toBool())
                        return;
                    socket->setProperty("rejected", true);
                    ++requests;
                    socket->readAll();
                    socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: "
                                  "0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        const auto settings = QString("model_provider = \"t3-offline\"\n"
                                      "[model_providers.t3-offline]\n"
                                      "name = \"T3 deny sentinel\"\n"
                                      "base_url = \"http://127.0.0.1:%1/v1\"\n"
                                      "wire_api = \"responses\"\n")
                                  .arg(sentinel.serverPort());
        QVERIFY(writeFile(profile + "/config.toml", settings.toUtf8()));
        const auto fixture = QString::fromUtf8(KETPLUS_CODEX_OFFLINE_FIXTURE);
        const auto launchConfig = root + "/launch.json";
        QVERIFY(writeFile(launchConfig, QJsonDocument(QJsonObject{{"executable", executable},
                                                                  {"profile", profile},
                                                                  {"work", work},
                                                                  {"log", log}})
                                            .toJson()));
        const auto wrapper = root + "/codex-control";
        const auto script =
            QString(
                "#!%1\nimport os,sys\nos.execv(%2,[%2,%3,'--launch-config',%4,*sys.argv[1:]])\n")
                .arg(python, quoted(python), quoted(fixture), quoted(launchConfig));
        QVERIFY(writeFile(wrapper, script.toUtf8(), true));
        QProcess version;
        version.start(wrapper, {"--version"});
        QVERIFY(version.waitForFinished(5000));
        const auto installedVersion = version.readAllStandardOutput().trimmed();
        QCOMPARE(installedVersion, "codex-cli 0.162.1");

        CodexAdapter adapter(wrapper);
        adapter.setBrowserArgumentsProvider([] { return QStringList{}; });
        Cleanup cleanup{adapter, log};
        auto setup = [&](const QString& epoch, const QString& thread = QString{}) {
            const agent_tools::Launch launch{python, "unused-fixture-socket", "synthetic-read",
                                             "synthetic-mutation-" + epoch, epoch};
            auto config = agent_tools::codexConfig(launch);
            config.insert("mcp_servers.ketplus_agents.args",
                          QJsonArray{fixture, "--worker-log", log});
            return QJsonObject{{"managedSessionId", "synthetic-owner"},
                               {"managedTransportEpoch", epoch},
                               {"threadId", thread},
                               {"cwd", work},
                               {"approvalPolicy", "never"},
                               {"sandbox", "read-only"},
                               {"config", config}};
        };
        const auto first = control(adapter, "thread/start", setup("one"));
        QVERIFY2(!first.contains("probe_error"), qPrintable(first.value("probe_error").toString()));
        QCOMPARE(first.value("managedTransportEpoch"), "one");
        const auto thread = first.value("thread").toObject().value("id").toString();
        QVERIFY(!thread.isEmpty());
        QVERIFY(inventory(adapter, thread).contains("t3_epoch_one"));
        const QByteArray marker("T3 synthetic injected context alpha");
        const auto injected = control(
            adapter, "thread/inject_items",
            {{"threadId", thread},
             {"items",
              QJsonArray{QJsonObject{
                  {"type", "message"},
                  {"role", "assistant"},
                  {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                                     {"text", QString::fromUtf8(marker)}}}}}}}});
        QVERIFY2(!injected.contains("probe_error"),
                 qPrintable(injected.value("probe_error").toString()));
        QTRY_VERIFY(rolloutContains(profile, thread, marker));
        auto sameParams = setup("one", thread);
        sameParams.remove("managedSessionId");
        sameParams.remove("managedTransportEpoch");
        const auto same = control(adapter, "thread/resume", sameParams);
        QVERIFY2(!same.contains("probe_error"), qPrintable(same.value("probe_error").toString()));
        QCOMPARE(same.value("thread").toObject().value("id"), thread);
        const auto before =
            control(adapter, "thread/read", {{"threadId", thread}, {"includeTurns", true}});
        QVERIFY(!before.contains("probe_error"));

        const auto refreshed = control(adapter, "thread/resume", setup("two", thread));
        QVERIFY2(!refreshed.contains("probe_error"),
                 qPrintable(refreshed.value("probe_error").toString()));
        QCOMPARE(refreshed.value("managedTransportEpoch"), "two");
        QCOMPARE(refreshed.value("thread").toObject().value("id"), thread);
        const auto tools = inventory(adapter, thread);
        QVERIFY(tools.contains("t3_epoch_two"));
        QVERIFY(!tools.contains("t3_epoch_one"));
        const auto after =
            control(adapter, "thread/read", {{"threadId", thread}, {"includeTurns", true}});
        QVERIFY(!after.contains("probe_error"));
        QVERIFY(rolloutContains(profile, thread, marker));
        QList<QJsonObject> providers;
        for (const auto& event : events(log)) {
            if (event.value("event") == "provider")
                providers.append(event);
            QVERIFY(event.value("event") != "unexpected_tool");
        }
        QCOMPARE(providers.size(), 2);
        QCOMPARE(providers.last().value("prior_mcp_alive").toInt(), 0);
        QVERIFY(providers.first().value("pid") != providers.last().value("pid"));
        QVERIFY(!alive(providers.first().value("pid").toInteger()));
        QVERIFY(adapter.releaseSessionTransport("synthetic-owner"));
        adapter.stop();
        QTRY_VERIFY(workersExited(log));
        QCOMPARE(requests, 0);
        qInfo().noquote() << QJsonDocument(
                                 QJsonObject{{"provider", "codex-cli 0.162.1"},
                                             {"production_adapter", true},
                                             {"same_id_resume", true},
                                             {"mcp_epoch_rotated", true},
                                             {"old_mcp_exited_before_new_provider", true},
                                             {"provider_written_marker", true},
                                             {"rendered_marker_before",
                                              QJsonDocument(before).toJson().contains(marker)},
                                             {"rendered_marker_after",
                                              QJsonDocument(after).toJson().contains(marker)},
                                             {"model_endpoint_requests", requests},
                                             {"remaining_owned_processes", 0},
                                             {"completed_turn_history_tested", false}})
                                 .toJson(QJsonDocument::Compact);
    }
};
QTEST_GUILESS_MAIN(CodexOfflineRefreshTest)
#include "CodexOfflineRefreshTest.moc"
