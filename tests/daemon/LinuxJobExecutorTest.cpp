#include "runtime/ManagedTaskRuntime.h"
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeProcessIdentity.h"
#include "storage/BackgroundTaskLedger.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <QtTest>
#include <csignal>

using namespace ketplus::daemon;
namespace {
class Fixture final {
  public:
    QString base;
    QString connection{QUuid::createUuid().toString()};
    QSqlDatabase database{QSqlDatabase::addDatabase("QSQLITE", connection)};
    BackgroundTaskLedger ledger{database};
    std::unique_ptr<ManagedTaskRuntime> runtime;
    explicit Fixture(QString path) : base(std::move(path)) {}
    ~Fixture() {
        runtime.reset();
        database.close();
    }
    bool initialize(QString* error) {
        database.setDatabaseName(base + "/journal.sqlite");
        if (!database.open() || !ledger.initialize(error))
            return false;
        QSqlQuery query(database);
        if (!query.exec(
                "CREATE TABLE IF NOT EXISTS sessions(id TEXT PRIMARY KEY,generation INTEGER)") ||
            !query.exec("INSERT OR IGNORE INTO sessions VALUES('owner',1)"))
            return false;
        runtime = std::make_unique<ManagedTaskRuntime>(ledger, base + "/jobs",
                                                       QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH));
        runtime->setCallbacks({},
                              [this](const QJsonObject& task, const QJsonObject&) {
                                  runtime->permissionResolved(task.value("task_id").toString(),
                                                              true);
                              },
                              {}, {});
        return runtime->initialize(error);
    }
    BackgroundTaskIntent intent(QString key = "start", qint64 timeout = 20000) const {
        return {{"owner", 1, "turn"}, key,     "process",      "Linux fixture",
                key.toUtf8(),         timeout, {{"cwd", base}}};
    }
    QJsonObject start(const BackgroundTaskIntent& intent, const QJsonObject& execution) {
        QJsonObject reply;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        deadline.start(10000);
        runtime->start(intent, execution, [&](QJsonObject result) {
            reply = std::move(result);
            loop.quit();
        });
        if (reply.isEmpty())
            loop.exec();
        return reply;
    }
    QJsonObject execution(const QJsonArray& argv) const {
        return {{"argv", argv}, {"cwd", base}, {"env_delta", QJsonObject{}}};
    }
    QJsonObject task(const QString& id) const { return ledger.runtimeTask(id); }
};
QString idOf(const QJsonObject& reply) {
    return reply.value("task").toObject().value("task_id").toString();
}
QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool gone(const QJsonObject& task) {
    const auto boundary =
        task.value("execution_identity").toObject().value("linux_boundary").toObject();
    // The manager can remove an empty subtree immediately after receipt ACK.
    // Check absence last so that removal during the identity read is also clean.
    return !boundary.isEmpty() && (executor::linuxBoundaryEmpty(boundary) ||
                                   !QFile::exists(boundary.value("path").toString()));
}
} // namespace

class LinuxJobExecutorTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        if (qEnvironmentVariable("KETPLUS_RUN_LINUX_JOB_TESTS") != "1")
            QSKIP("Opt-in: use an exclusively owned delegated Linux reference guest.");
        QString error;
        QVERIFY2(
            executor::linuxContainmentAvailable(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH), &error),
            qPrintable(error));
    }
    void missingManagerRejectsBeforeAcceptance() {
        QTemporaryDir directory;
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        const auto bus = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        const auto restore = qScopeGuard([&] { qputenv("DBUS_SESSION_BUS_ADDRESS", bus); });
        qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/ketplus-test-bus");
        const auto result = fixture.start(fixture.intent(), fixture.execution({"/usr/bin/true"}));
        QCOMPARE(result.value("error_code"), "containment_unavailable");
        QVERIFY(fixture.ledger.nonterminal().value("tasks").toArray().isEmpty());
    }
    void stalledAdmissionDoesNotExpireAnotherExecutorsLease() {
        QTemporaryDir directory;
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        const auto id = idOf(fixture.start(fixture.intent("healthy", 60000),
                                           fixture.execution({"/bin/sleep", "60"})));
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "running", 10000);
        const auto socket = directory.filePath("stalled-bus");
        const auto connected = directory.filePath("bus-connected");
        QProcess stalledBus;
        // An independent POSIX listener avoids using the Qt socket accept path
        // under test as the deliberately nonresponsive service-manager fixture.
        stalledBus.start("/usr/bin/python3", {"-c", R"PY(import socket, sys
server = socket.socket(socket.AF_UNIX)
server.bind(sys.argv[1])
server.listen()
print('ready', flush=True)
clients = []
while True:
    clients.append(server.accept()[0])
    open(sys.argv[2], 'w').close()
)PY",
                                              socket, connected});
        const auto stopBus = qScopeGuard([&] {
            stalledBus.kill();
            stalledBus.waitForFinished(2000);
        });
        QTRY_VERIFY(stalledBus.bytesAvailable() > 0);
        const auto bus = qgetenv("DBUS_SESSION_BUS_ADDRESS");
        const auto restore = qScopeGuard([&] { qputenv("DBUS_SESSION_BUS_ADDRESS", bus); });
        qputenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + socket).toUtf8());
        int beats = 0;
        QTimer timer;
        timer.setInterval(100);
        connect(&timer, &QTimer::timeout, [&] { ++beats; });
        timer.start();
        QElapsedTimer elapsed;
        elapsed.start();
        const auto rejected =
            fixture.start(fixture.intent("stalled"), fixture.execution({"/usr/bin/true"}));
        qputenv("DBUS_SESSION_BUS_ADDRESS", bus);
        QCOMPARE(rejected.value("error_code"), "containment_unavailable");
        QVERIFY(elapsed.elapsed() >= 5500); // Longer than the supervisor's 5 s lease.
        QVERIFY(beats > 20);
        QVERIFY(QFile::exists(connected));
        QTest::qWait(500); // Allow any genuine lease-loss notification to arrive.
        QCOMPARE(fixture.task(id).value("state"), "running");
        QCOMPARE(fixture.ledger.nonterminal().value("tasks").toArray().size(), 1);
        QVERIFY(fixture.runtime->stop(id, {"owner", 1, {}}).value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "cancelled", 10000);
        QVERIFY(gone(fixture.task(id)));
    }

    void durableStartRetryOutputAndVerifiedCompletion() {
        QTemporaryDir directory;
        qputenv("KETPLUS_FIXTURE_INHERITED", "from-daemon");
        qputenv("KETPLUS_FIXTURE_REMOVE", "remove-me");
        const auto restore = qScopeGuard([] {
            qunsetenv("KETPLUS_FIXTURE_INHERITED");
            qunsetenv("KETPLUS_FIXTURE_REMOVE");
        });
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        const auto request = fixture.intent();
        auto execution = fixture.execution(
            {"/bin/sh", "-c",
             "printf once >> marker; "
             "printf '%s/%s/%s' \"$KETPLUS_FIXTURE_INHERITED\" \"$KETPLUS_FIXTURE_DELTA\" "
             "\"${KETPLUS_FIXTURE_REMOVE-unset}\""});
        execution.insert("env_delta", QJsonObject{{"KETPLUS_FIXTURE_DELTA", "delta"},
                                                  {"KETPLUS_FIXTURE_REMOVE", QJsonValue::Null}});
        const auto started = fixture.start(request, execution);
        QVERIFY2(started.value("ok").toBool(),
                 qPrintable(started.value("error_message").toString()));
        const auto id = idOf(started);
        QCOMPARE(idOf(fixture.start(request, execution)), id);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "completed", 10000);
        QCOMPARE(fixture.task(id).value("containment_mode"), "cgroup_v2");
        QCOMPARE(fixture.task(id).value("cleanup_status"), "verified");
        QCOMPARE(fixture.runtime->output(id, {"owner", 1, {}}, {}).value("output"),
                 "from-daemon/delta/unset");
        QVERIFY(!read(directory.filePath("journal.sqlite")).contains("from-daemon"));
        QCOMPARE(read(directory.filePath("marker")), "once");
        QVERIFY(gone(fixture.task(id)));
        QCOMPARE(idOf(fixture.start(request, execution)), id);
        QCOMPARE(fixture.ledger.deliveryPage(false).value("events").toArray().size(), 1);
    }
    void detachedDescendantsAndMigrationAttemptsStayInsideBoundary() {
        QTemporaryDir directory;
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        // PID 1 exits after creating a double-forked, setsid descendant. Its
        // namespace and the cgroup must both be empty before a terminal result.
        QVERIFY(write(directory.filePath("worker.py"), R"PY(import os, time, json, ctypes
assert os.getpid() == 1
while not os.path.exists('boundary.json'): time.sleep(.01)
with open('boundary.json') as stream: boundary = json.load(stream)
attempts = {}
outside = os.path.dirname(boundary['path']) + '/cgroup.procs'
for path in [outside, '/proc/' + str(boundary['supervisor']) + '/root' + outside]:
    try:
        with open(path, 'w') as stream: stream.write(str(os.getpid()))
        attempts[path] = 'allowed'
    except OSError: attempts[path] = 'denied'
assert 'allowed' not in attempts.values(), attempts
with open('/sys/fs/cgroup/cgroup.procs', 'w') as stream: stream.write(str(os.getpid()))
os.mkdir('new-cgroup-mount')
libc = ctypes.CDLL(None, use_errno=True)
assert libc.mount(b'none', b'new-cgroup-mount', b'cgroup2', 0, None) == 0
try:
    with open('new-cgroup-mount/' + outside.removeprefix('/sys/fs/cgroup/'), 'w') as stream:
        stream.write(str(os.getpid()))
    raise AssertionError('migration through a new cgroup mount succeeded')
except OSError: pass
if os.fork() == 0:
    os.setsid()
    if os.fork() == 0:
        while True: time.sleep(1)
    os._exit(0)
time.sleep(.3)
print(json.dumps(attempts), flush=True)
)PY"));
        const auto id = idOf(fixture.start(
            fixture.intent(),
            fixture.execution({"/usr/bin/python3", directory.filePath("worker.py")})));
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "running", 10000);
        const auto identity = fixture.task(id).value("execution_identity").toObject();
        auto boundary = identity.value("linux_boundary").toObject();
        boundary.insert("supervisor",
                        identity.value("supervisor_identity").toObject().value("pid"));
        QVERIFY(write(directory.filePath("boundary.json"), QJsonDocument(boundary).toJson()));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "completed", 10000);
        QVERIFY(gone(fixture.task(id)));
        QCOMPARE(fixture.task(id).value("cleanup_status"), "verified");
    }
    void timeoutReclaimsConcurrentForks() {
        QTemporaryDir directory;
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        QElapsedTimer elapsed;
        elapsed.start();
        QJsonObject milestones;
        QJsonObject countSample{{"sampled", false}};
        QString id;
        const auto diagnostics = qScopeGuard([&] {
            if (!QTest::currentTestFailed() &&
                !qEnvironmentVariableIsSet("KETPLUS_MANAGED_TASK_DIAGNOSTICS"))
                return;
            const auto record = fixture.task(id);
            QJsonObject summary{{"elapsed_ms", elapsed.elapsed()},
                                {"milestones", milestones},
                                {"count_assertion_sample", countSample}};
            for (const auto* key : {"state", "reason", "created_at", "updated_at", "finished_at",
                                    "timeout_ms", "exit_code", "cleanup_status"})
                summary.insert(key, record.value(key));
            qWarning().noquote() << "Managed fork timeout timing:"
                                 << QJsonDocument(summary).toJson(QJsonDocument::Compact);
        });
        QVERIFY(write(directory.filePath("fork.py"), R"PY(import os, time, signal
signal.signal(signal.SIGTERM, signal.SIG_IGN)
for index in range(300):
    with open('fork-count', 'w') as stream: stream.write(str(index + 1))
    if os.fork() == 0:
        os.setsid()
        while True: time.sleep(.1)
    time.sleep(.02)
while True: time.sleep(.1)
)PY"));
        milestones.insert("before_task_start", elapsed.elapsed());
        id = idOf(
            fixture.start(fixture.intent("timeout", 2500),
                          fixture.execution({"/usr/bin/python3", directory.filePath("fork.py")})));
        milestones.insert("accepted", elapsed.elapsed());
        QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!read(directory.filePath("fork-count")).isEmpty(), 6000);
        milestones.insert("first_fork_marker", elapsed.elapsed());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "timed_out", 12000);
        milestones.insert("timed_out", elapsed.elapsed());
        const auto countBytes = read(directory.filePath("fork-count"));
        bool countParsed = false;
        const auto count = countBytes.toInt(&countParsed);
        const auto recordAtCount = fixture.task(id);
        const auto boundaryAtCount =
            recordAtCount.value("execution_identity").toObject().value("linux_boundary").toObject();
        countSample = {
            {"sampled", true},
            {"elapsed_ms", elapsed.elapsed()},
            {"count", count},
            {"parse_ok", countParsed},
            {"byte_size", countBytes.size()},
            {"bytes_hex", QString::fromLatin1(countBytes.left(64).toHex())},
            {"boundary_present", !boundaryAtCount.isEmpty()},
            {"boundary_empty", executor::linuxBoundaryEmpty(boundaryAtCount)},
            {"boundary_path_absent", !boundaryAtCount.isEmpty() &&
                                         !QFile::exists(boundaryAtCount.value("path").toString())},
            {"cleanup_status", recordAtCount.value("cleanup_status")}};
        QVERIFY(count > 1 && count < 300);
        QVERIFY(gone(fixture.task(id)));
        QCOMPARE(fixture.task(id).value("cleanup_status"), "verified");
    }
    void stopCompleteRaceHasOneImmutableTerminalAndOutbox() {
        QTemporaryDir directory;
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        for (int index = 0; index < 4; ++index) {
            const auto id =
                idOf(fixture.start(fixture.intent(QString::number(index)),
                                   fixture.execution({"/bin/sh", "-c", "sleep .1; exit 0"})));
            QVERIFY(!id.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(fixture.task(id).value("state") != "queued", 6000);
            if (index > 0) {
                QTRY_VERIFY_WITH_TIMEOUT(fixture.task(id).value("state") == "running" ||
                                             fixture.task(id).value("ended_at").isDouble(),
                                         6000);
                if (index == 2)
                    QTest::qWait(120);
            }
            QVERIFY(fixture.runtime->stop(id, {"owner", 1, {}}).value("ok").toBool());
            QTRY_VERIFY_WITH_TIMEOUT(fixture.task(id).value("ended_at").isDouble(), 10000);
            const auto result = fixture.task(id);
            QVERIFY(result.value("state") == "cancelled" || result.value("state") == "completed");
            if (result.value("cleanup_status") == "not_required") {
                QCOMPARE(result.value("state"), "cancelled");
                QVERIFY(result.value("execution_identity")
                            .toObject()
                            .value("process_birth_identity")
                            .toString()
                            .isEmpty());
            } else {
                QCOMPARE(result.value("cleanup_status"), "verified");
            }
            QVERIFY(gone(result));
            fixture.runtime->stop(id, {"owner", 1, {}});
            QCOMPARE(fixture.task(id).value("revision"), result.value("revision"));
        }
        QSqlQuery query(fixture.database);
        QVERIFY(query.exec("SELECT COUNT(*),COUNT(DISTINCT task_id) FROM managed_job_outbox"));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 4);
        QCOMPARE(query.value(1).toInt(), 4);
    }
    void supervisorSigkillUsesManagerCleanupAndNeverReplays() {
        QTemporaryDir directory;
        Fixture fixture(directory.path());
        QString error;
        QVERIFY(fixture.initialize(&error));
        const auto id = idOf(
            fixture.start(fixture.intent(),
                          fixture.execution({"/bin/sh", "-c", "printf once >> marker; sleep 30"})));
        QVERIFY(!id.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "running", 10000);
        const auto identity = fixture.task(id)
                                  .value("execution_identity")
                                  .toObject()
                                  .value("supervisor_identity")
                                  .toObject();
        QVERIFY(executor::signalIdentity(
            {identity.value("pid").toInteger(), 0, 0, identity.value("birth").toString()}, SIGKILL,
            &error));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.task(id).value("state"), "interrupted", 15000);
        QVERIFY(gone(fixture.task(id)));
        QCOMPARE(read(directory.filePath("marker")), "once");
        QCOMPARE(fixture.task(id).value("cleanup_status"), "verified");
    }
    void daemonSigkillLeaseRecoveryAndReopenDoNotReplay() {
        QTemporaryDir directory;
        QProcess daemon;
        daemon.setProgram(QCoreApplication::applicationFilePath());
        daemon.setArguments({"--fixture-daemon", directory.path()});
        daemon.start();
        QVERIFY(daemon.waitForStarted());
        const auto cleanup = qScopeGuard([&] {
            if (daemon.state() != QProcess::NotRunning) {
                daemon.kill();
                daemon.waitForFinished(3000);
            }
        });
        QTRY_VERIFY_WITH_TIMEOUT(!read(directory.filePath("task-id")).isEmpty(), 15000);
        const auto id = QString::fromUtf8(read(directory.filePath("task-id")));
        daemon.kill();
        QVERIFY(daemon.waitForFinished(3000));
        // Let the independent helper's daemon lease expire before reopening.
        QTest::qWait(6200);
        Fixture reopened(directory.path());
        QString error;
        QVERIFY(reopened.initialize(&error));
        QTRY_COMPARE_WITH_TIMEOUT(reopened.task(id).value("state"), "interrupted", 15000);
        QVERIFY(gone(reopened.task(id)));
        QCOMPARE(read(directory.filePath("marker")), "once");
        QCOMPARE(reopened.task(id).value("cleanup_status"), "verified");
        QCOMPARE(reopened.ledger.deliveryPage(false).value("events").toArray().size(), 1);
    }
};

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().size() == 3 && application.arguments()[1] == "--fixture-daemon") {
        Fixture fixture(application.arguments()[2]);
        QString error;
        if (!fixture.initialize(&error))
            return 2;
        const auto id = idOf(
            fixture.start(fixture.intent("crash", 60000),
                          fixture.execution({"/bin/sh", "-c", "printf once >> marker; sleep 30"})));
        if (id.isEmpty())
            return 3;
        QTimer timer;
        timer.setInterval(50);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            if (fixture.task(id).value("state") == "running" &&
                !read(fixture.base + "/marker").isEmpty()) {
                write(fixture.base + "/task-id", id.toUtf8());
                timer.stop();
            }
        });
        timer.start();
        QTimer::singleShot(60000, &application, &QCoreApplication::quit);
        return application.exec();
    }
    LinuxJobExecutorTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "LinuxJobExecutorTest.moc"
