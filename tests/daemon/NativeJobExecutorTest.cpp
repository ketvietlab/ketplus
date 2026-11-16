#include "runtime/executor/NativeJobClient.h"
#include "runtime/executor/NativeJobControlAddress.h"
#include "runtime/executor/NativeProcessIdentity.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <csignal>

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

    ~Executor() {
        if (helper.state() == QProcess::NotRunning)
            return;
        if (socket.state() != QLocalSocket::ConnectedState)
            reconnect();
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
    bool start() {
        if (!directory.isValid())
            return false;
        helper.start(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), {path(), taskId, lease});
        if (!helper.waitForStarted())
            return false;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000) {
            if (reconnect())
                return !receive("ready").isEmpty();
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
                if (messages[i].value("event") == event)
                    return messages.takeAt(i);
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
        QCOMPARE(done.value("state"), "timed_out");
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
        QCOMPARE(done.value("state"), "timed_out");
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
