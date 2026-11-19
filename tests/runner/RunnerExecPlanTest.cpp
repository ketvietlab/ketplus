#include "exec/ExecutionPlan.h"
#include "exec/ExecutorDirectories.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <memory>

using namespace ketplus::runner;
using namespace ketplus::runner::exec;

namespace {

constexpr QFile::Permissions anyone = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                      QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                      QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;

QString real(const QString& path) { return QFileInfo(path).canonicalFilePath(); }

session::LaunchRequest request(const QString& workspace) {
    session::LaunchRequest launch;
    launch.jobId = QStringLiteral("job-1");
    launch.attempt = 1;
    launch.timeoutMs = 1000;
    launch.invocation.argv = QStringList{QStringLiteral("make"), QStringLiteral("test")};
    launch.workspace = workspace;
    launch.environment = {{QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin")},
                          {QStringLiteral("JOB"), QStringLiteral("1")}};
    return launch;
}

} // namespace

class RunnerExecPlanTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        m_temp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_temp->isValid());
        m_root = real(m_temp->path());
        QVERIFY(QDir(m_root).mkpath(QStringLiteral("ws/sub")));
        QVERIFY(QDir(m_root).mkpath(QStringLiteral("outside")));
    }
    void cleanup() { m_temp.reset(); }

    void launchRunsArgvOrShell() {
        const QString ws = m_root + QStringLiteral("/ws");
        PreparedLaunch argv = prepareLaunch(request(ws), {});
        QVERIFY2(argv.error.isEmpty(), qPrintable(argv.error));
        QCOMPARE(argv.execution.value("argv").toArray(),
                 QJsonArray({QStringLiteral("make"), QStringLiteral("test")}));
        QVERIFY(!argv.execution.contains("shell_command"));
        QCOMPARE(argv.execution.value("cwd").toString(), ws);

        session::LaunchRequest shell = request(ws);
        shell.invocation = {std::nullopt, QStringLiteral("make test | tee log")};
        const PreparedLaunch prepared = prepareLaunch(shell, {});
        QVERIFY(prepared.error.isEmpty());
        QCOMPARE(prepared.execution.value("shell_command").toString(),
                 QStringLiteral("make test | tee log"));
        QVERIFY(!prepared.execution.contains("argv"));
    }

    void launchRefusals_data() {
        QTest::addColumn<int>("change");
        QTest::addColumn<QString>("error");
        QTest::newRow("no invocation") << 0 << "invalid_invocation";
        QTest::newRow("argv and shell") << 1 << "invalid_invocation";
        QTest::newRow("empty argv") << 2 << "invalid_invocation";
        QTest::newRow("empty program") << 3 << "invalid_invocation";
        QTest::newRow("NUL in argument") << 4 << "invalid_invocation";
        QTest::newRow("empty shell") << 5 << "invalid_invocation";
        QTest::newRow("workspace missing") << 6 << "workspace_missing";
        QTest::newRow("workspace relative") << 7 << "workspace_missing";
        QTest::newRow("cwd absolute") << 8 << "cwd_outside_workspace";
        QTest::newRow("cwd dot-dot") << 9 << "cwd_outside_workspace";
        QTest::newRow("cwd symlink out") << 10 << "cwd_outside_workspace";
        QTest::newRow("cwd missing") << 11 << "cwd_missing";
        QTest::newRow("variable with =") << 12 << "invalid_environment";
        QTest::newRow("empty variable") << 13 << "invalid_environment";
        QTest::newRow("NUL in value") << 14 << "invalid_environment";
        QTest::newRow("too large") << 15 << "launch_too_large";
    }
    void launchRefusals() {
        QFETCH(int, change);
        QFETCH(QString, error);
        const QString ws = m_root + QStringLiteral("/ws");
        QVERIFY(QFile::link(m_root + QStringLiteral("/outside"), ws + QStringLiteral("/out")));
        session::LaunchRequest launch = request(ws);
        switch (change) {
        case 0:
            launch.invocation = {};
            break;
        case 1:
            launch.invocation.shell = QStringLiteral("true");
            break;
        case 2:
            launch.invocation.argv = QStringList{};
            break;
        case 3:
            launch.invocation.argv = QStringList{QString(), QStringLiteral("x")};
            break;
        case 4:
            launch.invocation.argv = QStringList{QStringLiteral("a") + QChar::Null};
            break;
        case 5:
            launch.invocation = {std::nullopt, QString()};
            break;
        case 6:
            launch.workspace = m_root + QStringLiteral("/none");
            break;
        case 7:
            launch.workspace = QStringLiteral("ws");
            break;
        case 8:
            launch.cwd = m_root + QStringLiteral("/outside");
            break;
        case 9:
            launch.cwd = QStringLiteral("sub/../../outside");
            break;
        case 10:
            launch.cwd = QStringLiteral("out");
            break;
        case 11:
            launch.cwd = QStringLiteral("absent");
            break;
        case 12:
            launch.environment.insert(QStringLiteral("A=B"), QStringLiteral("1"));
            break;
        case 13:
            launch.environment.insert(QString(), QStringLiteral("1"));
            break;
        case 14:
            launch.environment.insert(QStringLiteral("A"), QString(QChar::Null));
            break;
        case 15:
            launch.environment.insert(QStringLiteral("BIG"), QString(maxLaunchBytes, u'x'));
            break;
        }
        const PreparedLaunch prepared = prepareLaunch(launch, {});
        QCOMPARE(prepared.error, error);
        QVERIFY(prepared.execution.isEmpty());
    }

    void cwdInsideIsResolved() {
        const QString ws = m_root + QStringLiteral("/ws");
        QVERIFY(QFile::link(ws + QStringLiteral("/sub"), ws + QStringLiteral("/link")));
        session::LaunchRequest launch = request(ws);
        launch.cwd = QStringLiteral("link");
        PreparedLaunch prepared = prepareLaunch(launch, {});
        QVERIFY(prepared.error.isEmpty());
        QCOMPARE(prepared.execution.value("cwd").toString(), ws + QStringLiteral("/sub"));
        launch.cwd = QStringLiteral("sub/..");
        prepared = prepareLaunch(launch, {});
        QCOMPARE(prepared.execution.value("cwd").toString(), ws);
    }

    // The command's environment is the request's: the runner's own variables are removed.
    void environmentIsExactlyTheRequests() {
        const QString ws = m_root + QStringLiteral("/ws");
        const QMap<QString, QString> runner{{QStringLiteral("PATH"), QStringLiteral("/runner")},
                                            {QStringLiteral("RUNNER_SECRET"), QStringLiteral("s")},
                                            {QStringLiteral("HOME"), QStringLiteral("/home/r")}};
        const PreparedLaunch prepared = prepareLaunch(request(ws), runner);
        QVERIFY(prepared.error.isEmpty());
        const QJsonObject delta = prepared.execution.value("env_delta").toObject();
        QCOMPARE(delta.size(), 4);
        QCOMPARE(delta.value("PATH").toString(), QStringLiteral("/usr/bin:/bin"));
        QCOMPARE(delta.value("JOB").toString(), QStringLiteral("1"));
        QVERIFY(delta.value("RUNNER_SECRET").isNull());
        QVERIFY(delta.value("HOME").isNull());
    }

    void mountsHideTheRunner() {
        const QString state = m_root + QStringLiteral("/state");
        const QString work = m_root + QStringLiteral("/work");
        const QString runtime = m_root + QStringLiteral("/run-user");
        const QString tmp = work + QStringLiteral("/jobtmp/t-1");
        for (const QString& path : {state, work + QStringLiteral("/job/ws"), runtime, tmp})
            QVERIFY(QDir().mkpath(path));
        QVERIFY(QFile::link(work + QStringLiteral("/job/ws"), m_root + QStringLiteral("/alias")));

        const MountPlan plan =
            planMounts(state, work, runtime, m_root + QStringLiteral("/alias"), tmp);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        QCOMPARE(plan.hidden, QStringList({state, work, runtime}));
        QCOMPARE(plan.workspace, work + QStringLiteral("/job/ws"));
        QCOMPARE(plan.tmp, tmp);

        const MountPlan noRuntime =
            planMounts(state, work, QString(), work + QStringLiteral("/job/ws"), tmp);
        QVERIFY(noRuntime.error.isEmpty());
        QCOMPARE(noRuntime.hidden, QStringList({state, work}));
    }

    // The execution carries the plan as `mounts`; a plan that failed or lacks a part runs
    // nothing.
    void launchCarriesTheMounts() {
        const QString work = m_root + QStringLiteral("/work");
        const QString workspace = work + QStringLiteral("/job/ws");
        QVERIFY(QDir().mkpath(workspace));
        MountPlan plan;
        plan.hidden = {m_root + QStringLiteral("/state"), work};
        plan.workspace = workspace;
        plan.tmp = work + QStringLiteral("/jobtmp/t-1");
        const PreparedLaunch prepared = prepareLaunch(request(workspace), {}, &plan);
        QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
        const QJsonObject mounts = prepared.execution.value(QStringLiteral("mounts")).toObject();
        QCOMPARE(mounts.value(QStringLiteral("hidden")).toArray().size(), 2);
        QCOMPARE(mounts.value(QStringLiteral("workspace")).toString(), workspace);
        QCOMPARE(mounts.value(QStringLiteral("tmp")).toString(), plan.tmp);
        QVERIFY(
            !prepareLaunch(request(workspace), {}).execution.contains(QStringLiteral("mounts")));

        MountPlan failed = plan;
        failed.error = QStringLiteral("runner_directory_missing");
        QCOMPARE(prepareLaunch(request(workspace), {}, &failed).error, failed.error);
        MountPlan noTmp = plan;
        noTmp.tmp.clear();
        QCOMPARE(prepareLaunch(request(workspace), {}, &noTmp).error,
                 QStringLiteral("mounts_missing"));
        MountPlan otherWorkspace = plan;
        otherWorkspace.workspace = work + QStringLiteral("/job/other");
        QCOMPARE(prepareLaunch(request(workspace), {}, &otherWorkspace).error,
                 QStringLiteral("workspace_outside_work"));
    }

    void mountRefusals_data() {
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("work");
        QTest::addColumn<QString>("runtime");
        QTest::addColumn<QString>("workspace");
        QTest::addColumn<QString>("tmp");
        QTest::addColumn<QString>("error");
        QTest::newRow("state missing")
            << "none" << "work" << "" << "work/ws" << "work/tmp" << "runner_directory_missing";
        QTest::newRow("work missing")
            << "state" << "none" << "" << "work/ws" << "work/tmp" << "runner_directory_missing";
        QTest::newRow("outside work")
            << "state" << "work" << "" << "outside" << "work/tmp" << "workspace_outside_work";
        QTest::newRow("work itself")
            << "state" << "work" << "" << "work" << "work/tmp" << "workspace_outside_work";
        QTest::newRow("dot-dot out")
            << "state" << "work" << "" << "work/ws/../.." << "work/tmp" << "workspace_outside_work";
        QTest::newRow("symlink out")
            << "state" << "work" << "" << "work/escape" << "work/tmp" << "workspace_outside_work";
        QTest::newRow("in state") << "work/state" << "work" << "" << "work/state/ws"
                                  << "work/tmp" << "runtime_directory_invalid";
        QTest::newRow("missing") << "state" << "work" << "" << "work/none"
                                 << "work/tmp" << "workspace_outside_work";
        QTest::newRow("runtime missing") << "state" << "work" << "none" << "work/ws" << "work/tmp"
                                         << "runtime_directory_invalid";
        QTest::newRow("in runtime") << "state" << "work" << "work" << "work/ws" << "work/tmp"
                                    << "runtime_directory_invalid";
        // Any two of the state, work and runtime directories, one inside the other or the same.
        QTest::newRow("state in work") << "work/state" << "work" << "" << "work/ws" << "work/tmp"
                                       << "runtime_directory_invalid";
        QTest::newRow("work in state") << "state" << "state/work" << "" << "state/work/ws"
                                       << "state/work/tmp" << "runtime_directory_invalid";
        QTest::newRow("state is work")
            << "work" << "work" << "" << "work/ws" << "work/tmp" << "runtime_directory_invalid";
        QTest::newRow("runtime in work") << "state" << "work" << "work/run" << "work/ws"
                                         << "work/tmp" << "runtime_directory_invalid";
        QTest::newRow("work in runtime") << "state" << "run/work" << "run" << "run/work/ws"
                                         << "run/work/tmp" << "runtime_directory_invalid";
        QTest::newRow("runtime in state") << "state" << "work" << "state/run" << "work/ws"
                                          << "work/tmp" << "runtime_directory_invalid";
        QTest::newRow("state in runtime") << "run/state" << "work" << "run" << "work/ws"
                                          << "work/tmp" << "runtime_directory_invalid";
        QTest::newRow("runtime is state") << "state" << "work" << "state" << "work/ws" << "work/tmp"
                                          << "runtime_directory_invalid";
        QTest::newRow("runtime through a link into work")
            << "state" << "work" << "run-link" << "work/ws" << "work/tmp"
            << "runtime_directory_invalid";
        QTest::newRow("tmp missing")
            << "state" << "work" << "" << "work/ws" << "work/none" << "tmp_directory_invalid";
        QTest::newRow("tmp empty")
            << "state" << "work" << "" << "work/ws" << "" << "tmp_directory_invalid";
        QTest::newRow("tmp outside work")
            << "state" << "work" << "" << "work/ws" << "outside-tmp" << "tmp_directory_invalid";
        QTest::newRow("tmp is work")
            << "state" << "work" << "" << "work/ws" << "work" << "tmp_directory_invalid";
        QTest::newRow("tmp in workspace")
            << "state" << "work" << "" << "work/ws" << "work/ws/tmp" << "tmp_directory_invalid";
        QTest::newRow("workspace in tmp")
            << "state" << "work" << "" << "work/tmp/ws" << "work/tmp" << "tmp_directory_invalid";
        QTest::newRow("tmp symlink out")
            << "state" << "work" << "" << "work/ws" << "work/tmp-escape" << "tmp_directory_invalid";
    }

    void mountRefusals() {
        QFETCH(QString, state);
        QFETCH(QString, work);
        QFETCH(QString, runtime);
        QFETCH(QString, workspace);
        QFETCH(QString, tmp);
        QFETCH(QString, error);
        for (const char* path :
             {"state", "work/ws", "work/state/ws", "work/tmp/ws", "work/ws/tmp", "outside-tmp",
              "work/tmp", "work/run", "state/work/ws", "state/work/tmp", "state/run", "run/work/ws",
              "run/work/tmp", "run/state"})
            QVERIFY(QDir(m_root).mkpath(QLatin1String(path)));
        QVERIFY(QFile::link(m_root + QStringLiteral("/outside"),
                            m_root + QStringLiteral("/work/escape")));
        QVERIFY(QFile::link(m_root + QStringLiteral("/outside-tmp"),
                            m_root + QStringLiteral("/work/tmp-escape")));
        QVERIFY(QFile::link(m_root + QStringLiteral("/work/run"),
                            m_root + QStringLiteral("/run-link")));
        const auto at = [this](const QString& path) {
            return path.isEmpty() ? path : m_root + u'/' + path;
        };
        const MountPlan plan = planMounts(at(state), at(work), at(runtime), at(workspace), at(tmp));
        QCOMPARE(plan.error, error);
        QVERIFY(plan.hidden.isEmpty());
        QVERIFY(plan.workspace.isEmpty());
        QVERIFY(plan.tmp.isEmpty());
    }

    void taskIdHashesTheJob() {
        const QString jobId = QStringLiteral("../jobs/a.b/../c");
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(jobId.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
        QCOMPARE(executorTaskId(jobId, 3), hash + QStringLiteral("-3"));
        QVERIFY(!executorTaskId(jobId, 3).contains(u'/'));
        QVERIFY(!executorTaskId(jobId, 3).contains(u'.'));
        QVERIFY(executorTaskId(jobId, 1) != executorTaskId(jobId, 2));
        QVERIFY(executorTaskId(QString(), 1).isEmpty());
        QVERIFY(executorTaskId(jobId, 0).isEmpty());
    }

    void exitFromResult_data() {
        QTest::addColumn<QJsonObject>("result");
        QTest::addColumn<QString>("state");
        QTest::addColumn<qint64>("exitCode"); // -1: none
        QTest::addColumn<QString>("reason");
        QTest::addColumn<QString>("cleanup");
        const auto result = [](const char* state, bool started, int exitCode, const char* cleanup,
                               const char* error = "") {
            return QJsonObject{{"state", state},
                               {"root_started", started},
                               {"exit_code", exitCode},
                               {"cleanup_status", cleanup},
                               {"error", QString::fromLatin1(error)}};
        };
        QTest::newRow("exit 0") << result("completed", true, 0, "verified") << "exited" << qint64(0)
                                << "" << "verified";
        QTest::newRow("exit 3") << result("failed", true, 3, "verified") << "exited" << qint64(3)
                                << "" << "verified";
        QTest::newRow("linux signal") << result("failed", true, 137, "verified") << "exited"
                                      << qint64(137) << "" << "verified";
        QTest::newRow("macos abnormal") << result("failed", true, -1, "not_required") << "failed"
                                        << qint64(-1) << "exit_status_unknown" << "verified";
        QTest::newRow("cancelled")
            << result("cancelled", true, -1, "best_effort_complete") << "failed" << qint64(-1)
            << "exit_status_unknown" << "unverified";
        QTest::newRow("not started") << result("failed", false, -1, "not_required", "spawn_enoent")
                                     << "failed" << qint64(-1) << "spawn_enoent" << "verified";
        QTest::newRow("bad error code") << result("failed", false, -1, "verified", "No such file")
                                        << "failed" << qint64(-1) << "spawn_failed" << "verified";
        const QByteArray longest(64, 'e');
        QTest::newRow("error code 64")
            << result("failed", false, -1, "verified", longest.constData()) << "failed"
            << qint64(-1) << QString::fromLatin1(longest) << "verified";
        const QByteArray tooLong(65, 'e');
        for (const QByteArray& error :
             {QByteArray("0spawn"), QByteArray("_spawn"), QByteArray("Spawn_enoent"),
              QByteArray("spawn-enoent"), tooLong})
            QTest::addRow("bad error code %s", error.left(12).constData())
                << result("failed", false, -1, "verified", error.constData()) << "failed"
                << qint64(-1) << "spawn_failed" << "verified";
        QTest::newRow("backstop") << result("timed_out", true, 137, "verified") << "failed"
                                  << qint64(-1) << "timed_out" << "verified";
        QTest::newRow("interrupted") << result("interrupted", true, -1, "verified") << "failed"
                                     << qint64(-1) << "executor_interrupted" << "verified";
        QTest::newRow("needs attention") << result("needs_attention", true, 0, "verified")
                                         << "exited" << qint64(0) << "" << "unverified";
        QTest::newRow("unverified") << result("completed", true, 0, "unverified") << "exited"
                                    << qint64(0) << "" << "unverified";
        QTest::newRow("unknown cleanup")
            << result("completed", true, 0, "maybe") << "exited" << qint64(0) << "" << "unverified";
        QTest::newRow("no cleanup")
            << QJsonObject{{"state", "completed"}, {"root_started", true}, {"exit_code", 0}}
            << "exited" << qint64(0) << "" << "unverified";
    }
    void exitFromResult() {
        QFETCH(QJsonObject, result);
        QFETCH(QString, state);
        QFETCH(qint64, exitCode);
        QFETCH(QString, reason);
        QFETCH(QString, cleanup);
        const session::CommandExit exit = commandExit(result);
        QCOMPARE(exit.state, state);
        QCOMPARE(exit.exitCode, exitCode < 0 ? std::nullopt : std::optional<qint64>(exitCode));
        QVERIFY(!exit.signal);
        QCOMPARE(exit.reason, reason);
        QCOMPARE(exit.cleanupStatus, cleanup);
    }

    void directoriesHoldTheIdentity() {
        const QString root = m_root + QStringLiteral("/state/executor");
        ExecutorDirectories directories(root);
        const QString jobId = QStringLiteral("team/job.1/../x");
        const auto entry = directories.create(jobId, 2);
        QVERIFY(entry);
        QCOMPARE(entry->taskId, executorTaskId(jobId, 2));
        QCOMPARE(entry->path, root + u'/' + entry->taskId);
        QCOMPARE(entry->jobId, jobId);
        QVERIFY(!QUuid::fromString(entry->lease).isNull());
        const auto ownerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        QCOMPARE(QFileInfo(root).permissions() & anyone, ownerOnly);
        QCOMPARE(QFileInfo(entry->path).permissions() & anyone, ownerOnly);
        QVERIFY(!directories.create(jobId, 2)); // never twice

        QCOMPARE(directories.names(), QStringList{entry->taskId});
        const auto read = directories.read(entry->taskId);
        QVERIFY(read);
        QCOMPARE(read->jobId, jobId);
        QCOMPARE(read->attempt, 2);
        QCOMPARE(read->lease, entry->lease);
        QVERIFY(directories.remove(*read));
        QVERIFY(!QFileInfo::exists(entry->path));
    }

    void unidentifiedDirectories() {
        const QString root = m_root + QStringLiteral("/executor");
        ExecutorDirectories directories(root);
        const auto entry = directories.create(QStringLiteral("job-a"), 1);
        QVERIFY(entry);
        QVERIFY(!directories.read(QStringLiteral("stray")));
        QVERIFY(!directories.read(QStringLiteral("../executor")));

        // An identity of another job, under this job's name.
        const auto other = directories.create(QStringLiteral("job-b"), 1);
        QVERIFY(other);
        QFile identity(other->path + QStringLiteral("/identity.json"));
        QVERIFY(identity.open(QIODevice::WriteOnly | QIODevice::Truncate));
        identity.write(
            QJsonDocument(QJsonObject{{"jobId", "job-a"}, {"attempt", 1}, {"lease", entry->lease}})
                .toJson());
        identity.close();
        QVERIFY(!directories.read(other->taskId));

        // Readable by others.
        QVERIFY(QFile::setPermissions(entry->path, QFile::ReadOwner | QFile::WriteOwner |
                                                       QFile::ExeOwner | QFile::ReadOther |
                                                       QFile::ExeOther));
        QVERIFY(!directories.read(entry->taskId));

        // A root others may write to is not used.
        QVERIFY(QFile::setPermissions(root, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                                QFile::WriteOther));
        QVERIFY(directories.names().isEmpty());
        QVERIFY(!directories.create(QStringLiteral("job-c"), 1));
        QVERIFY(!ExecutorDirectories(QStringLiteral("relative")).create(QStringLiteral("j"), 1));
    }

  private:
    std::unique_ptr<QTemporaryDir> m_temp;
    QString m_root;
};

QTEST_GUILESS_MAIN(RunnerExecPlanTest)
#include "RunnerExecPlanTest.moc"
