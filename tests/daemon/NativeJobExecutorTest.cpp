#include "runtime/executor/NativeJobClient.h"
#include "runtime/executor/NativeJobControlAddress.h"
#include "runtime/executor/NativeProcessIdentity.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <csignal>
#ifdef Q_OS_MACOS
#include <libproc.h>
#endif

using namespace ketplus::daemon::executor;

namespace {
class Executor final {
  public:
    QTemporaryDir directory{QStringLiteral("/tmp/ketplus-job-XXXXXX")};
    QString taskId{QUuid::createUuid().toString(QUuid::WithoutBraces)};
    QString lease{QUuid::createUuid().toString(QUuid::WithoutBraces)};
    QProcess helper;
    QLocalSocket socket;
    QByteArray buffer;
    QList<QJsonObject> messages;
    bool doneReceived{false};
    bool helperStarted{false};
    QJsonObject startupDiagnostics;

    ~Executor() {
        if (helper.state() == QProcess::NotRunning)
            return;
        if (socket.state() != QLocalSocket::ConnectedState)
            reconnect();
        if (doneReceived) {
            send({{"op", "ack"}});
            if (helper.waitForFinished(1500))
                return;
        }
        send({{"op", "stop"}, {"reason", "cancelled"}});
        const auto done = receive("done", 4500);
        if (!done.isEmpty())
            send({{"op", "ack"}});
        if (!helper.waitForFinished(1500)) {
            // Test-owned receipt identities only, with the production birth check.
            const auto metadata = receipt();
            for (const auto value : metadata.value("known_processes").toArray()) {
                const auto identity = value.toObject();
                QString error;
                signalIdentity(
                    {identity.value("pid").toInteger(), 0, 0, identity.value("birth").toString()},
                    SIGKILL, &error);
            }
            helper.terminate();
            if (!helper.waitForFinished(4000)) {
                helper.kill();
                helper.waitForFinished();
            }
        }
    }

    QString path() const { return QFileInfo(directory.path()).canonicalFilePath(); }
    QJsonObject receipt() const {
        QFile file(path() + "/executor.json");
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    QJsonObject diagnosticState() const {
        const bool exited = helperStarted && helper.state() == QProcess::NotRunning;
        return {{"process_state", int(helper.state())},
                {"process_error", int(helper.error())},
                {"exit_information_valid", exited},
                {"exit_status", exited ? QJsonValue(int(helper.exitStatus())) : QJsonValue()},
                {"exit_code", exited ? QJsonValue(helper.exitCode()) : QJsonValue()},
                {"socket_state", int(socket.state())},
                {"socket_error", int(socket.error())}};
    }
    bool start() {
        QElapsedTimer elapsed;
        elapsed.start();
        QString phase = "directory";
        bool readyReceived = false;
        const auto capture = qScopeGuard([&] {
            startupDiagnostics = diagnosticState();
            startupDiagnostics.insert("phase", phase);
            startupDiagnostics.insert("elapsed_ms", elapsed.elapsed());
            startupDiagnostics.insert("directory_valid", directory.isValid());
            startupDiagnostics.insert("process_started", helperStarted);
            startupDiagnostics.insert("ready_received", readyReceived);
        });
        if (!directory.isValid())
            return false;
        phase = "process_start";
        helper.start(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), {path(), taskId, lease});
        if (!helper.waitForStarted())
            return false;
        helperStarted = true;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000) {
            phase = "connect";
            if (reconnect()) {
                phase = "ready";
                readyReceived = !receive("ready").isEmpty();
                return readyReceived;
            }
            QTest::qWait(20);
        }
        return false;
    }
    bool reconnect() {
        socket.abort();
        buffer.clear();
        messages.clear();
        socket.connectToServer(controlAddress(path(), lease));
        if (!socket.waitForConnected(100))
            return false;
        send({{"op", "auth"}, {"lease_id", lease}});
        return true;
    }
    void send(const QJsonObject& value) {
        if (socket.state() == QLocalSocket::ConnectedState) {
            socket.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
            socket.flush();
        }
    }
    QJsonObject receive(const QString& event, int timeout = 5000) {
        QElapsedTimer timer;
        timer.start();
        qint64 pulse = 0;
        while (timer.elapsed() < timeout) {
            for (qsizetype i = 0; i < messages.size(); ++i)
                if (messages[i].value("event") == event) {
                    if (event == "done")
                        doneReceived = true;
                    return messages.takeAt(i);
                }
            if (timer.elapsed() - pulse > 750) {
                send({{"op", "ping"}});
                pulse = timer.elapsed();
            }
            socket.waitForReadyRead(20);
            buffer.append(socket.readAll());
            for (auto end = buffer.indexOf('\n'); end >= 0; end = buffer.indexOf('\n')) {
                messages.append(QJsonDocument::fromJson(buffer.left(end)).object());
                buffer.remove(0, end + 1);
            }
            QCoreApplication::processEvents();
        }
        return {};
    }
    void execute(const QJsonArray& argv, int timeout = 30000) {
        send({{"op", "start"},
              {"execution",
               QJsonObject{{"argv", argv}, {"cwd", path()}, {"env_delta", QJsonObject{}}}},
              {"remaining_timeout_ms", timeout}});
    }
};
} // namespace

class NativeJobExecutorTest final : public QObject {
    Q_OBJECT

  private slots:
    void stoppingBeforeTheIdentityCommitNeverExecutesWorkload() {
        Executor executor;
        QVERIFY(executor.start());
        const auto marker = executor.path() + "/must-not-run";
        const QJsonObject start{
            {"op", "start"},
            {"execution", QJsonObject{{"argv", QJsonArray{"/bin/sh", "-c", "touch must-not-run"}},
                                      {"cwd", executor.path()},
                                      {"env_delta", QJsonObject{}}}},
            {"remaining_timeout_ms", 10000}};
        const QJsonObject stop{{"op", "stop"}, {"reason", "cancelled"}, {"lease", executor.lease}};
        auto authorized = start;
        authorized.insert("lease", executor.lease);
        // One frame batch is consumed before the pre-exec notifier can run.
        executor.socket.write(QJsonDocument(authorized).toJson(QJsonDocument::Compact) + '\n' +
                              QJsonDocument(stop).toJson(QJsonDocument::Compact) + '\n');
        executor.socket.flush();
        const auto done = executor.receive("done");
        QCOMPARE(done.value("state"), "cancelled");
        QVERIFY(!done.value("root_started").toBool());
        QVERIFY(!QFileInfo::exists(marker));
    }

    void shortLivedCommandsHaveDurableBirthIdentityBeforeExec() {
        for (int index = 0; index < 12; ++index) {
            Executor executor;
            const auto diagnostics = qScopeGuard([&] {
                if (!QTest::currentTestFailed() &&
                    !qEnvironmentVariableIsSet("KETPLUS_MANAGED_TASK_DIAGNOSTICS"))
                    return;
                // This guard runs before Executor's destructor can reconnect or stop it.
                auto summary = executor.startupDiagnostics;
                summary.insert("iteration", index + 1);
                summary.insert("before_cleanup", executor.diagnosticState());
                QFile metadata(executor.path() + "/executor.json");
                bool readable = false;
                if (metadata.open(QIODevice::ReadOnly)) {
                    const auto bytes = metadata.read(128 * 1024);
                    QJsonParseError error;
                    const auto document = QJsonDocument::fromJson(bytes, &error);
                    readable = metadata.atEnd() && error.error == QJsonParseError::NoError &&
                               document.isObject();
                }
                summary.insert("receipt_readable", readable);
                qWarning().noquote() << "Native helper bootstrap:"
                                     << QJsonDocument(summary).toJson(QJsonDocument::Compact);
            });
            QVERIFY(executor.start());
            executor.execute({"/usr/bin/true"});
            const auto done = executor.receive("done");
            QCOMPARE(done.value("state"), "completed");
            QCOMPARE(done.value("cleanup_status"), "best_effort_complete");
            QVERIFY(done.value("launch_gate_committed").toBool());
            QVERIFY(!done.value("root_identity").toObject().value("birth").toString().isEmpty());
            QVERIFY(executor.receipt().value("launch_gate_committed").toBool());
        }
    }

    void timeoutReclaimsObservedDetachedDescendantByBirthIdentity() {
        Executor executor;
        QVERIFY(executor.start());
        executor.execute({QStringLiteral(KETPLUS_JOB_WORKLOAD_PATH)}, 1500);
        const auto running = executor.receive("running");
        QVERIFY(!running.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(executor.receipt().value("known_processes").toArray().size() >= 2,
                                 1000);
        const auto before = executor.receipt().value("known_processes").toArray();
        const auto root = running.value("root_identity").toObject();
        bool detached = false;
        for (const auto value : before)
            if (value.toObject().value("process_group") != root.value("process_group"))
                detached = true;
        QVERIFY(detached);
        const auto done = executor.receive("done");
        QVERIFY2(done.value("state") == "timed_out", qPrintable(QJsonDocument(done).toJson()));
        QCOMPARE(done.value("cleanup_status"), "best_effort_complete");
        for (const auto value : before) {
            const auto old = value.toObject();
            const auto current = inspect(old.value("pid").toInteger());
            QVERIFY(!current || current->birth != old.value("birth").toString() || current->zombie);
        }
    }

    void supervisorCrashReclaimsRecordedRootAndPreservesUnverifiedAttention() {
        Executor fixture;
        QVERIFY(fixture.start());
        fixture.execute({"/bin/sleep", "60"});
        const auto running = fixture.receive("running");
        const auto root = running.value("root_identity").toObject();
        QVERIFY(!root.isEmpty());
        fixture.socket.abort();
        fixture.helper.kill();
        QVERIFY(fixture.helper.waitForFinished());
        NativeJobClient recovered(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), fixture.path(),
                                  fixture.taskId, fixture.lease);
        QJsonObject done;
        QString error;
        QVERIFY(recovered.reconnect(
            [&](const QJsonObject& value) {
                if (value.value("state") == "needs_attention")
                    done = value;
            },
            &error));
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() && done.value("executor_stopped").toBool(), 10000);
        QCOMPARE(done.value("state"), "needs_attention");
        QCOMPARE(done.value("cleanup_status"), "unverified");
        const auto current = inspect(root.value("pid").toInteger());
        QVERIFY(!current || current->birth != root.value("birth").toString() || current->zombie);
        // Result publication does not turn best-effort recovery into kernel evidence.
        recovered.acknowledge();
    }

    void daemonClientCommitsHandshakeBeforeLaunchAndReconnectNeverReplays() {
        Executor fixture;
        const auto execution = QJsonObject{{"argv", QJsonArray{"/bin/sleep", "2"}},
                                           {"cwd", fixture.path()},
                                           {"env_delta", QJsonObject{}}};
        QJsonObject done;
        QJsonObject ready;
        QString error;
        int handshakes = 0;
        {
            NativeJobClient client(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), fixture.path(),
                                   fixture.taskId, fixture.lease);
            QVERIFY2(client.start(
                         execution, 10000,
                         [&](const QJsonObject& receipt) {
                             ++handshakes;
                             ready = receipt;
                             // The workload cannot precede the durable host handshake.
                             return !receipt.value("launch_attempted").toBool() &&
                                    receipt.value("root_identity").toObject().isEmpty();
                         },
                         [&](const QJsonObject& value) {
                             if (value.value("event") == "done")
                                 done = value;
                         },
                         &error),
                     qPrintable(error));
            QTRY_VERIFY_WITH_TIMEOUT(fixture.receipt().value("root_started").toBool(), 3000);
            QCOMPARE(handshakes, 1);
        } // Simulated daemon connection loss; the independent supervisor stays alive.
        NativeJobClient recovered(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), fixture.path(),
                                  fixture.taskId, fixture.lease);
        QVERIFY2(recovered.reconnect(
                     [&](const QJsonObject& value) {
                         if (value.value("event") == "done")
                             done = value;
                     },
                     &error),
                 qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty(), 5000);
        QCOMPARE(done.value("state"), "completed");
        QCOMPARE(done.value("cleanup_status"), "best_effort_complete");
        QCOMPARE(handshakes, 1);
        recovered.acknowledge();
        const auto supervisor = ready.value("supervisor_identity").toObject();
        QTRY_VERIFY_WITH_TIMEOUT(!inspect(supervisor.value("pid").toInteger()) ||
                                     inspect(supervisor.value("pid").toInteger())->zombie,
                                 3000);
    }

    void daemonClientRefusesWorkloadWhenHostHandshakeCannotCommit() {
        Executor fixture;
        NativeJobClient client(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), fixture.path(),
                               fixture.taskId, fixture.lease);
        QJsonObject result;
        QString error;
        QVERIFY(client.start(
            {{"argv", QJsonArray{"/bin/sleep", "60"}}, {"cwd", fixture.path()}}, 10000,
            [](const QJsonObject&) { return false; },
            [&](const QJsonObject& value) {
                if (value.value("event") == "done")
                    result = value;
            },
            &error));
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 3000);
        QCOMPARE(result.value("state"), "interrupted");
        QVERIFY(!result.value("launch_attempted").toBool());
        QVERIFY(result.value("root_identity").toObject().isEmpty());
        QCOMPARE(result.value("cleanup_status"), "not_required");
        client.acknowledge();
    }

    void runtimeCheckReportsTheSqliteDriver() {
        QProcess check;
        check.start(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), {"--check-runtime"});
        QVERIFY(check.waitForFinished(10000));
        QCOMPARE(check.exitCode(), 0);
        QCOMPARE(check.readAllStandardOutput().trimmed(), "ketplus-job-executor runtime ok");
    }

    // The output spool opens before the control socket. A failure there launched nothing, so
    // the receipt says failed/handshake_failed and the client ends the job.
    void outputSpoolFailureLeavesAFailedHandshakeReceipt() {
        Executor fixture;
        QVERIFY(QDir(fixture.path()).mkdir("output.sqlite"));
        NativeJobClient client(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), fixture.path(),
                               fixture.taskId, fixture.lease);
        QJsonObject result;
        QString error;
        QVERIFY2(client.start(
                     {{"argv", QJsonArray{"/bin/sleep", "60"}}, {"cwd", fixture.path()}}, 30000,
                     [](const QJsonObject&) { return true; },
                     [&](const QJsonObject& value) {
                         if (value.value("event") == "done")
                             result = value;
                     },
                     &error),
                 qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 10000);
        QCOMPARE(result.value("state"), "failed");
        QCOMPARE(result.value("error"), "handshake_failed");
        QVERIFY(!result.value("root_started").toBool());
        QCOMPARE(fixture.receipt().value("state"), "failed");
        client.acknowledge();
    }

    // No receipt and no control socket: the supervisor never reached the point where a
    // workload could start.
    void supervisorWithoutReceiptIsReportedNeverStarted() {
        Executor fixture;
        NativeJobClient client(QStringLiteral("/usr/bin/false"), fixture.path(), fixture.taskId,
                               fixture.lease);
        QJsonObject result;
        QString error;
        QVERIFY2(client.start(
                     {{"argv", QJsonArray{"/bin/sleep", "60"}}, {"cwd", fixture.path()}}, 30000,
                     [](const QJsonObject&) { return true; },
                     [&](const QJsonObject& value) {
                         if (value.value("event") == "done")
                             result = value;
                     },
                     &error),
                 qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 10000);
        QCOMPARE(result.value("state"), "failed");
        QCOMPARE(result.value("cleanup_issue"), "executor_never_started");
        QVERIFY(result.value("executor_stopped").toBool());
        client.acknowledge();
    }

    void capturedMacSignalCannotTargetAChangedProcessIncarnation() {
#ifdef Q_OS_MACOS
        QProcess process;
        process.start("/bin/sh", {"-c", "printf ready; read line; exec /bin/sleep 60"});
        const auto cleanup = qScopeGuard([&] {
            process.kill();
            process.waitForFinished(2000);
        });
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForReadyRead());
        QCOMPARE(process.readAllStandardOutput(), "ready");
        const auto identity = inspect(process.processId());
        QVERIFY(identity);
        QString error;
        const auto captured = NativeProcessSignal::capture(*identity, &error);
        QVERIFY2(captured, qPrintable(error));
        process.write("exec\n");
        process.waitForBytesWritten();
        const auto execed = [&] {
            char path[PROC_PIDPATHINFO_MAXSIZE]{};
            return proc_pidpath(int(process.processId()), path, sizeof(path)) > 0 &&
                   QByteArray(path).endsWith("/sleep");
        };
        QTRY_VERIFY(execed());
        // exec changes the kernel PID version while preserving PID and birth.
        // This deterministically exercises lookup-to-signal incarnation change.
        QCOMPARE(inspect(process.processId())->birth, identity->birth);
        QVERIFY2(captured->send(SIGTERM, &error), qPrintable(error));
        QTest::qWait(150);
        QCOMPARE(process.state(), QProcess::Running);
        QVERIFY2(signalIdentity(*inspect(process.processId()), SIGTERM, &error), qPrintable(error));
        QVERIFY(process.waitForFinished(2000));
#endif
    }

    void handshakeDoesNotLaunchAndNormalExitIsBestEffort() {
        Executor executor;
        QVERIFY(executor.start());
        QVERIFY(!executor.receipt().value("launch_attempted").toBool());
        executor.execute({"/usr/bin/printf", "hello"});
        const auto running = executor.receive("running");
        QVERIFY2(!running.isEmpty(), executor.helper.readAllStandardError().constData());
        const auto done = executor.receive("done");
        QCOMPARE(done.value("state"), "completed");
        QCOMPARE(done.value("exit_code"), 0);
        QCOMPARE(done.value("containment_mode"), "native_best_effort");
        QCOMPARE(done.value("cleanup_status"), "best_effort_complete");
        QVERIFY(done.value("executor_stopped").toBool());
        executor.send({{"op", "ack"}});
        QVERIFY(executor.helper.waitForFinished(2000));
    }

    void reconnectAndRepeatedStartDoNotLaunchAgain() {
        Executor executor;
        QVERIFY(executor.start());
        const QJsonArray command{"/bin/sleep", "2"};
        executor.execute(command);
        const auto first = executor.receive("running").value("root_identity").toObject();
        QVERIFY(!first.isEmpty());
        executor.socket.disconnectFromServer();
        QTest::qWait(100);
        QVERIFY(executor.reconnect());
        QVERIFY(!executor.receive("ready").isEmpty());
        executor.execute(command);
        const auto again = executor.receive("running").value("root_identity").toObject();
        QCOMPARE(again, first);
        QCOMPARE(executor.receive("done").value("state"), "completed");
    }

    void timeoutReclaimsKnownChildrenBeforeReporting() {
        Executor executor;
        QVERIFY(executor.start());
        executor.execute({"/bin/sh", "-c", "sleep 60 & wait"}, 600);
        QVERIFY(!executor.receive("running").isEmpty());
        QTest::qWait(250);
        const auto before = executor.receipt().value("known_processes").toArray();
        QVERIFY(before.size() >= 2);
        const auto done = executor.receive("done");
        QVERIFY2(done.value("state") == "timed_out", qPrintable(QJsonDocument(done).toJson()));
        QCOMPARE(done.value("cleanup_status"), "best_effort_complete");
        QVERIFY(done.value("known_processes").toArray().isEmpty());
        for (const auto value : before) {
            const auto old = value.toObject();
            const auto current = inspect(old.value("pid").toInteger());
            QVERIFY(!current || current->birth != old.value("birth").toString() || current->zombie);
        }
    }

    void leaseLossLeavesAttentionReceiptAndStopsKnownRoot() {
        Executor executor;
        QVERIFY(executor.start());
        executor.execute({"/bin/sleep", "60"});
        const auto running = executor.receive("running");
        const auto root = running.value("root_identity").toObject();
        QVERIFY(!root.isEmpty());
        executor.socket.abort();
        QVERIFY(executor.helper.waitForFinished(8500));
        const auto receipt = executor.receipt();
        QCOMPARE(receipt.value("state"), "needs_attention");
        QCOMPARE(receipt.value("cleanup_status"), "unverified");
        QVERIFY(receipt.value("executor_stopped").toBool());
        const auto current = inspect(root.value("pid").toInteger());
        QVERIFY(!current || current->birth != root.value("birth").toString() || current->zombie);
    }

    void changedBirthNeverSignalsTheNewProcess() {
        QProcess process;
        process.start("/bin/sleep", {"60"});
        QVERIFY(process.waitForStarted());
        const auto captured = inspect(process.processId());
        QVERIFY(captured.has_value());
        auto reused = *captured;
        reused.birth += "-different";
        QString error;
        QVERIFY(!signalIdentity(reused, SIGKILL, &error));
        QCOMPARE(process.state(), QProcess::Running);
        process.kill();
        QVERIFY(process.waitForFinished());
    }
};

QTEST_GUILESS_MAIN(NativeJobExecutorTest)
#include "NativeJobExecutorTest.moc"
