#include "DaemonClient.h"
#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "daemon/FakeCodex.h"
#include "ipc/LocalIpcServer.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/LocalJournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace ketplus;
using namespace ketplus::daemon;

class ManagedBackgroundEndToEndTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
#ifdef Q_OS_LINUX
        if (qEnvironmentVariable("KETPLUS_RUN_LINUX_JOB_TESTS") != "1")
            QSKIP("Use an exclusively owned delegated Linux reference guest.");
#endif
    }

    void registryEventsReachDesktopAndWakeOnlyTheirOwner() {
        QStandardPaths::setTestModeEnabled(true);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto cwd = QFileInfo(directory.path()).canonicalFilePath();
        const auto otherCwd = directory.filePath("other");
        QVERIFY(QDir().mkpath(otherCwd));
        const auto providerLog = directory.filePath("provider.log");
        QVERIFY(writeFakeCodex(directory.filePath("fake-codex"), providerLog));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        for (const auto& id : {QString("owner"), QString("other")}) {
            StoredSession stored;
            stored.id = id;
            stored.worktreePath = id == "owner" ? cwd : otherCwd;
            stored.adapter = "codex";
            stored.state = "ready";
            stored.executionPolicy = "full";
            QVERIFY(journal.upsertSession(stored));
        }
        CodexAdapter provider(directory.filePath("fake-codex"));
        SessionHost host(&journal, &provider);
        const auto stopProvider = qScopeGuard([&] { provider.stop(); });
        auto ipc = std::make_unique<LocalIpcServer>(&host, directory.filePath("ipc.sock"),
                                                    QByteArray(40, 't'));
        QVERIFY2(ipc->listen(), qPrintable(ipc->errorString()));
        const auto tokenPath = directory.filePath("ipc.token");
        QFile token(tokenPath);
        QVERIFY(token.open(QIODevice::WriteOnly));
        QVERIFY(token.setPermissions(QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(token.write(QByteArray(40, 't')), 40);
        token.close();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("KETPLUS_DAEMON_SOCKET", ipc->fullServerName().toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", tokenPath.toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");

        CodexAppServer desktop;
        AgentSessionManager manager(&desktop);
        remote::DaemonClient controller;
        QTRY_VERIFY(desktop.hasDaemonSession("owner"));
        QTRY_VERIFY(controller.isReady());
        manager.activate(cwd, false);
        const QJsonObject spec{
            {"client_request_id", "e2e-request"},
            {"kind", "process"},
            {"title", "End-to-end fixture"},
            {"timeout_ms", 10000},
            {"execution_spec",
             QJsonObject{{"cwd", cwd},
                         {"argv", QJsonArray{"/bin/sh", "-c", "printf managed-output:ok; sleep 1"}},
                         {"env_delta", QJsonObject{}}}}};
        QJsonObject accepted;
        QString startError;
        controller.call("session.task.start", spec, "owner", 1,
                        [&](const QJsonObject& result, const QString& error) {
                            accepted = result;
                            startError = error;
                        });
        QTRY_VERIFY(!accepted.isEmpty() || !startError.isEmpty());
        QVERIFY2(startError.isEmpty(), qPrintable(startError));
        QVERIFY(accepted.value("accepted").toBool());
        const auto id = accepted.value("task").toObject().value("task_id").toString();
        QVERIFY(!id.isEmpty());
        const auto desktopRecord = [&](const QString& taskId) {
            for (const auto& entry : manager.backgroundTasks(cwd))
                if (entry.toMap().value("task_id").toString() == taskId)
                    return entry.toMap();
            return QVariantMap{};
        };
        const auto desktopTask = [&] { return desktopRecord(id); };
        QTRY_VERIFY(!desktopTask().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(desktopTask().value("state").toString(), "completed", 10000);
        QTRY_COMPARE(desktopTask().value("completion").toMap().value("status").toString(),
                     "delivered");
        QCOMPARE(desktopTask().value("status"), desktopTask().value("state"));
        QVERIFY(manager.backgroundTaskState(cwd).value("canRead").toBool());
        QJsonObject output;
        QString outputError;
        desktop.call("task/output", {{"threadId", "owner"}, {"taskId", id}},
                     [&](const QJsonObject& result, const QString& error) {
                         output = result;
                         outputError = error;
                     });
        QTRY_VERIFY(!output.isEmpty() || !outputError.isEmpty());
        QVERIFY2(outputError.isEmpty(), qPrintable(outputError));
        QCOMPARE(output.value("output").toString(), "managed-output:ok");

        QJsonObject retry;
        controller.call("session.task.start", spec, "owner", 1,
                        [&](const QJsonObject& result, const QString&) { retry = result; });
        QTRY_VERIFY(!retry.isEmpty());
        QCOMPARE(retry.value("task").toObject().value("task_id").toString(), id);
        const auto event = desktopTask().value("completion_event_id").toString();
        QVERIFY(!event.isEmpty());
        QVERIFY(journal.backgroundTasks()
                    .acceptCompletion(event, {"owner", 1, {}})
                    .value("duplicate")
                    .toBool());
        QTest::qWait(1200);
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        int turns = 0;
        for (const auto& line : log.readAll().split('\n'))
            turns += line.startsWith("turn:");
        QCOMPARE(turns, 1);
        int ownerWakes = 0;
        for (const auto& value : journal.eventsAfter("owner", 1, 0))
            ownerWakes += value.toObject().value("payload").toObject().value(
                              "background_task_event_id") == event;
        QCOMPARE(ownerWakes, 1);
        for (const auto& value : journal.eventsAfter("other", 1, 0))
            QVERIFY(
                !value.toObject().value("payload").toObject().contains("background_task_event_id"));

        auto cancellable = spec;
        cancellable.insert("client_request_id", "e2e-stop-request");
        cancellable.insert("execution_spec", QJsonObject{{"cwd", cwd},
                                                         {"argv", QJsonArray{"/bin/sleep", "60"}},
                                                         {"env_delta", QJsonObject{}}});
        QJsonObject second;
        controller.call("session.task.start", cancellable, "owner", 1,
                        [&](const QJsonObject& result, const QString&) { second = result; });
        QTRY_VERIFY(!second.isEmpty());
        QVERIFY(second.value("accepted").toBool());
        const auto cancelledId = second.value("task").toObject().value("task_id").toString();
        QVERIFY(!cancelledId.isEmpty());
        QTRY_COMPARE(desktopRecord(cancelledId).value("state").toString(), "running");
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool answered = false;
            QString stopError;
            desktop.call("task/stop", {{"threadId", "owner"}, {"taskId", cancelledId}},
                         [&](const QJsonObject&, const QString& error) {
                             stopError = error;
                             answered = true;
                         });
            QTRY_VERIFY(answered);
            QVERIFY2(stopError.isEmpty(), qPrintable(stopError));
        }
        QTRY_COMPARE(desktopRecord(cancelledId).value("state").toString(), "cancelled");
        QTRY_COMPARE(
            desktopRecord(cancelledId).value("completion").toMap().value("status").toString(),
            "suppressed");
        QTest::qWait(1200);
        QVERIFY(log.seek(0));
        turns = 0;
        for (const auto& line : log.readAll().split('\n'))
            turns += line.startsWith("turn:");
        QCOMPARE(turns, 1);

        manager.activate(otherCwd, false);
        QTRY_VERIFY(manager.backgroundTasks(otherCwd).isEmpty());
        manager.activate(cwd, false);
        QTRY_VERIFY(!desktopTask().isEmpty());
        ipc.reset();
        QTRY_VERIFY(!desktop.isReady());
        QString offlineError;
        desktop.call("task/stop", {{"threadId", "owner"}, {"taskId", id}},
                     [&](const QJsonObject&, const QString& error) { offlineError = error; });
        QVERIFY(!offlineError.isEmpty());
        QVERIFY(!manager.backgroundTaskState(cwd).value("canControl").toBool());
    }
};

QTEST_MAIN(ManagedBackgroundEndToEndTest)
#include "ManagedBackgroundEndToEndTest.moc"
