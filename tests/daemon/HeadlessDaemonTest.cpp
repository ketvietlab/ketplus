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
#include <QProcess>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

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
} // namespace
class HeadlessDaemonTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
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
