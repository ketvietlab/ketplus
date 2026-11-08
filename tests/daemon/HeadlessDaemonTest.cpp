#include "DaemonClient.h"
#include "DaemonPaths.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMap>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace ketplus::daemon;
namespace {
QJsonObject command(const QString& type, const QString& path = {}) {
    return {{"protocol_version", protocol::currentVersion},
            {"message_id", QUuid::createUuid().toString()},
            {"message_type", type},
            {"payload", QJsonObject{{"path", path}}}};
}
bool initializeGit(const QString& path) {
    return QProcess::execute("git", {"-C", path, "init", "-q"}) == 0;
}

void credentialCases() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("valid");
    for (const auto& kind : {"generated", "legacy32", "limit", "padded-limit"})
        QTest::newRow(kind) << QString(kind) << true;
    for (const auto& kind : {"oversized", "oversized-trimmed", "short", "empty", "directory"})
        QTest::newRow(kind) << QString(kind) << false;
#ifdef Q_OS_UNIX
    for (const auto& kind : {"group-readable", "symlink", "dangling-symlink", "fifo"})
        QTest::newRow(kind) << QString(kind) << false;
#endif
}
QByteArray credentialBytes(const QString& kind) {
    if (kind == "legacy32")
        return QByteArray(32, 't');
    if (kind == "limit")
        return QByteArray(4096, 't');
    if (kind == "padded-limit")
        return QByteArray(4051, ' ') + QByteArray(43, 't') + "\r\n";
    if (kind == "oversized")
        return QByteArray(4097, 't');
    if (kind == "oversized-trimmed")
        return QByteArray(4052, ' ') + QByteArray(43, 't') + "\r\n";
    if (kind == "short")
        return QByteArray(31, 't');
    if (kind == "empty")
        return QByteArray(4096, ' ');
    return QByteArray(43, 't');
}
bool writeCredential(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
           file.write(bytes) == bytes.size();
}
bool prepareCredential(const QString& path, const QString& kind) {
#ifdef Q_OS_UNIX
    if (kind == "fifo")
        return ::mkfifo(QFile::encodeName(path).constData(), 0600) == 0;
#endif
    if (kind == "directory")
        return QDir().mkdir(path);
    if (kind == "symlink" || kind == "dangling-symlink") {
        const auto target = path + ".target";
        if (kind == "symlink" && !writeCredential(target, credentialBytes(kind)))
            return false;
        return QFile::link(target, path);
    }
    if (!writeCredential(path, credentialBytes(kind)))
        return false;
    return kind != "group-readable" ||
           QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                           QFileDevice::ReadGroup);
}
} // namespace
class HeadlessDaemonTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void serviceCredentialAdmission_data() { credentialCases(); }
    void serviceCredentialAdmission() {
        QFETCH(QString, kind);
        QFETCH(bool, valid);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("local.token");
        QVERIFY(prepareCredential(path, kind));
        const QFileInfo before(path);
        const bool wasLink = before.isSymLink(), wasDirectory = before.isDir();
        const qint64 sizeBefore = before.size();
        const auto targetBefore = wasLink ? before.symLinkTarget() : QString{};
        const auto permissionsBefore = before.permissions();
        const DaemonConfiguration config{directory.filePath("journal.sqlite3"),
                                         path,
                                         directory.filePath("daemon.sock"),
                                         "/nonexistent/codex",
                                         "/nonexistent/claude",
                                         "/nonexistent/cursor"};
        {
            DaemonService service(config);
            QCOMPARE(service.start(), valid);
            if (!valid) {
                QVERIFY(!service.errorString().isEmpty());
                QVERIFY(!QFileInfo::exists(config.statePath));
                const QFileInfo after(path);
                QCOMPARE(after.isSymLink(), wasLink);
                QCOMPARE(after.isDir(), wasDirectory);
                QCOMPARE(after.size(), sizeBefore);
                QCOMPARE(after.permissions(), permissionsBefore);
                if (wasLink)
                    QCOMPARE(after.symLinkTarget(), targetBefore);
            }
        }
        if (!valid) {
            if (kind == "directory")
                QVERIFY(QDir().rmdir(path));
            else
                QVERIFY(QFile::remove(path));
            QVERIFY(writeCredential(path, QByteArray(43, 'r')));
            DaemonService repaired(config);
            QVERIFY2(repaired.start(), qPrintable(repaired.errorString()));
        }
    }

    void clientCredentialAdmission_data() { credentialCases(); }
    void clientCredentialAdmission() {
        QFETCH(QString, kind);
        QFETCH(bool, valid);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("local.token");
        QVERIFY(prepareCredential(path, kind));
        const auto endpoint = directory.filePath("fake.sock");
        QMap<QByteArray, QByteArray> saved;
        for (const auto& key : {"KETPLUS_DAEMON_TOKEN_FILE", "KETPLUS_DAEMON_SOCKET",
                                "KETPLUS_DISABLE_DAEMON_AUTOSTART"})
            saved.insert(key, qgetenv(key));
        const auto restore = qScopeGuard([&] {
            for (auto it = saved.cbegin(); it != saved.cend(); ++it)
                it.value().isNull() ? qunsetenv(it.key().constData())
                                    : qputenv(it.key().constData(), it.value());
        });
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", path.toUtf8());
        qputenv("KETPLUS_DAEMON_SOCKET", endpoint.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        QList<QJsonObject> received;
        int closed = 0;
        QLocalServer server;
        QVERIFY(server.listen(endpoint));
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            while (auto* socket = server.nextPendingConnection()) {
                const auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QLocalSocket::disconnected, &server, [&, socket] {
                    ++closed;
                    socket->deleteLater();
                });
                connect(socket, &QLocalSocket::readyRead, &server, [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (buffer->size() < 4)
                        return;
                    quint32 size = 0;
                    for (int i = 0; i < 4; ++i)
                        size = (size << 8U) | static_cast<uchar>(buffer->at(i));
                    if (buffer->size() < 4 + qsizetype(size))
                        return;
                    received.append(QJsonDocument::fromJson(buffer->mid(4, size)).object());
                    buffer->remove(0, 4 + size);
                });
            }
        });
        ketplus::remote::DaemonClient client;
        client.call("session.list", {}, {}, -1, [](const QJsonObject&, const QString&) {});
        QTRY_VERIFY_WITH_TIMEOUT(closed > 0 || !received.isEmpty(), 3000);
        if (!valid) {
            QVERIFY(received.isEmpty());
            QVERIFY(closed > 0);
            if (kind == "directory")
                QVERIFY(QDir().rmdir(path));
            else
                QVERIFY(QFile::remove(path));
            QVERIFY(writeCredential(path, QByteArray(43, 'r')));
            QTRY_VERIFY_WITH_TIMEOUT(!received.isEmpty(), 3000);
        }
        const auto hello = received.first();
        QCOMPARE(hello.value("message_type").toString(), QString("daemon.hello"));
        const auto expected = valid ? credentialBytes(kind).trimmed() : QByteArray(43, 'r');
        // Do not put credential bytes into assertion diagnostics, even in fixtures.
        QVERIFY(hello.value("payload").toObject().value("token").toString().toUtf8() == expected);
    }

    void workspaceCatalogExistsBeforeAnyConversationAndSurvivesRestart() {
        QTemporaryDir directory;
        QVERIFY(initializeGit(directory.path()));
        const auto path = QFileInfo(directory.path()).canonicalFilePath();
        const auto database = directory.filePath("journal.db");
        {
            LocalJournal journal(database);
            CodexAdapter adapter("/nonexistent/ketplus-test-codex");
            SessionHost host(&journal, &adapter);
            QVERIFY(journal.sessions().isEmpty());
            QVERIFY(host.handleCommand(command("workspace.register", path), "admin")
                        .value("ok")
                        .toBool());
            QVERIFY(host.handleCommand(command("workspace.register", path), "admin")
                        .value("ok")
                        .toBool());
            QCOMPARE(journal.worktreePaths(), QStringList{path});
        }
        LocalJournal journal(database);
        CodexAdapter adapter("/nonexistent/ketplus-test-codex");
        SessionHost host(&journal, &adapter);
        const auto entries =
            host.handleCommand(command("workspace.list"), "admin").value("workspaces").toArray();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().toObject().value("path").toString(), path);
        QVERIFY(host.handleCommand(command("workspace.unregister", path), "admin")
                    .value("ok")
                    .toBool());
        QVERIFY(journal.worktreePaths().isEmpty());
    }
    void workspaceEnrollmentRequiresALocalAdministratorAndARealCheckout() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.db"));
        CodexAdapter adapter("/nonexistent/ketplus-test-codex");
        SessionHost host(&journal, &adapter);
        QVERIFY(!host.handleCommand(command("workspace.register", directory.path()), "admin")
                     .value("ok")
                     .toBool());
        QVERIFY(initializeGit(directory.path()));
        QVERIFY(
            !host.handleCommand(command("workspace.register", "."), "admin").value("ok").toBool());
        QVERIFY(
            !host.handleCommand(command("workspace.register", directory.path()), "cloud-control-v2")
                 .value("ok")
                 .toBool());
        QVERIFY(journal.worktreePaths().isEmpty());
        StoredSession session;
        session.id = "retained-session";
        session.worktreePath = QFileInfo(directory.path()).canonicalFilePath();
        session.adapter = "codex";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        const auto refused =
            host.handleCommand(command("workspace.unregister", directory.path()), "admin");
        QCOMPARE(refused.value("error_code").toString(), "workspace_in_use");
    }
    void clientAndServiceUseTheSameConfiguredPaths() {
        const auto previous = qgetenv("KETPLUS_DAEMON_DATA_DIR");
        const auto restore = qScopeGuard([&] {
            previous.isNull() ? qunsetenv("KETPLUS_DAEMON_DATA_DIR")
                              : qputenv("KETPLUS_DAEMON_DATA_DIR", previous);
        });
        QTemporaryDir directory;
        qputenv("KETPLUS_DAEMON_DATA_DIR", directory.path().toUtf8());
        const auto configuration = DaemonService::defaultConfiguration();
        QCOMPARE(configuration.socketName, ketplus::remote::DaemonPaths::socket());
        QCOMPARE(configuration.statePath, directory.filePath("daemon/journal.sqlite3"));
        QCOMPARE(configuration.tokenPath, directory.filePath("daemon/local.token"));
    }
    void administratorCliAuthenticatesAndSigtermLeavesTheJournalUsable() {
#if defined(Q_OS_UNIX)
        QTemporaryDir directory("/tmp/kp-headless-XXXXXX");
        QVERIFY(directory.isValid());
        QVERIFY(initializeGit(directory.path()));
        QVERIFY(QDir().mkpath(directory.filePath("daemon")));
        {
            LocalJournal journal(directory.filePath("daemon/journal.sqlite3"));
            QVERIFY(journal.isOpen());
            QSqlDatabase db;
            for (const auto& name : QSqlDatabase::connectionNames())
                if (QSqlDatabase::database(name).databaseName() == journal.databasePath())
                    db = QSqlDatabase::database(name);
            QSqlQuery q(db);
            QVERIFY(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                           "WHERE i<30000) INSERT INTO session_tombstones "
                           "SELECT 'deleted-'||i,1,1,'operation-'||i,datetime('now') FROM n"));
        }
        QProcess daemon;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("KETPLUS_DAEMON_DATA_DIR", directory.path());
        environment.insert("KETPLUS_DAEMON_SOCKET", directory.filePath("socket"));
        daemon.setProcessEnvironment(environment);
        daemon.start(KETPLUS_TEST_DAEMON,
                     {"--codex", "/nonexistent/codex", "--claude", "/nonexistent/claude",
                      "--cursor", "/nonexistent/agent"});
        QVERIFY(daemon.waitForStarted());
        const auto stop = qScopeGuard([&] {
            if (daemon.state() != QProcess::NotRunning) {
                daemon.kill();
                daemon.waitForFinished();
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath("daemon/local.token")), 5000);
        QProcess cli;
        cli.setProcessEnvironment(environment);
        cli.start(KETPLUS_TEST_CTL, {"--timeout", "5", "workspace", "add", directory.path()});
        QVERIFY(cli.waitForFinished(8000));
        QCOMPARE(cli.exitCode(), 0);
        QVERIFY(QJsonDocument::fromJson(cli.readAllStandardOutput()).object().value("ok").toBool());
        cli.start(KETPLUS_TEST_CTL, {"--timeout", "15", "sessions"});
        QVERIFY(cli.waitForFinished(20000));
        QCOMPARE(cli.exitCode(), 0);
        const auto catalog = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
        QVERIFY(catalog.value("ok").toBool());
        QVERIFY(catalog.value("sessions").isArray());
        QCOMPARE(catalog.value("deleted_sessions").toArray().size(), 30000);
        QSet<QString> identities;
        for (const auto& row : catalog.value("deleted_sessions").toArray())
            identities.insert(row.toObject().value("session_id").toString());
        QCOMPARE(identities.size(), 30000);
        // The local credential is mandatory even with access to the same socket.
        QFile wrong(directory.filePath("wrong.token"));
        QVERIFY(wrong.open(QIODevice::WriteOnly));
        wrong.write(QByteArray(64, 'x'));
        wrong.close();
        cli.start(KETPLUS_TEST_CTL,
                  {"--timeout", "1", "--token-file", wrong.fileName(), "workspace", "list"});
        QVERIFY(cli.waitForFinished(4000));
        QVERIFY(cli.exitCode() != 0);
        daemon.terminate();
        QVERIFY(daemon.waitForFinished(5000));
        QCOMPARE(daemon.exitStatus(), QProcess::NormalExit);
        QCOMPARE(daemon.exitCode(), 0);
        LocalJournal journal(directory.filePath("daemon/journal.sqlite3"));
        QCOMPARE(journal.worktreePaths(),
                 QStringList{QFileInfo(directory.path()).canonicalFilePath()});
#endif
    }
};
QTEST_GUILESS_MAIN(HeadlessDaemonTest)
#include "HeadlessDaemonTest.moc"
