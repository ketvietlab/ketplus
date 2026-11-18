#include "exec/ExecutionPlan.h"
#include "exec/ExecutorCommandRunner.h"
#include "exec/ExecutorDirectories.h"
#include "exec/LinuxPolicy.h"
#include "exec/LinuxSystemHost.h"
#include "exec/NativeExecutor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QUuid>

#include <memory>

// The runner's command runner on Linux with the real job executor, its Linux policy and the job's
// mounts. It needs a user session with delegated cgroup v2 and unprivileged user namespaces, so
// it runs only with KETPLUS_RUN_LINUX_JOB_TESTS=1.
using namespace ketplus::runner;
using namespace ketplus::runner::exec;

namespace {

const QString program = QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH);

// Processes, seen from this side, whose command line holds `marker`.
int processesWith(const QByteArray& marker) {
    int found = 0;
    for (const QString& pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs)) {
        if (pid.toLongLong() <= 0)
            continue;
        QFile file(QStringLiteral("/proc/") + pid + QStringLiteral("/cmdline"));
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(marker))
            ++found;
    }
    return found;
}

QString key(const char c) { return QString(32, QLatin1Char(c)); }

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QStringList logged;
void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
    logged.append(message);
}

// A runner's directories like the real ones: state, work with two workspaces of two jobs.
struct Layout {
    explicit Layout(const QString& parent)
        : temp(parent + QStringLiteral("/ketplus-linux-job-XXXXXX")) {
        base = QFileInfo(temp.path()).canonicalFilePath();
        state = base + QStringLiteral("/state");
        work = base + QStringLiteral("/work");
        workspace = work + u'/' + key('a') + u'/' + key('b');
        other = work + u'/' + key('c') + u'/' + key('d');
        for (const QString& path : {state, workspace + QStringLiteral("/sub"), other})
            QDir().mkpath(path);
        writeFile(state + QStringLiteral("/secret"), "state");
        writeFile(other + QStringLiteral("/secret"), "other");
    }

    QTemporaryDir temp;
    QString base, state, work, workspace, other;
};

struct Linux {
    explicit Linux(const QString& parent = QDir::homePath()) : layout(parent) { runner = make(); }

    std::unique_ptr<ExecutorCommandRunner> make() {
        ExecutorCommandRunner::Options options;
        options.directory = layout.state + QStringLiteral("/executor");
        options.mounts = ExecutorCommandRunner::Mounts{layout.state, layout.work,
                                                       qEnvironmentVariable("XDG_RUNTIME_DIR")};
        ExecutorCommandRunner::Events events;
        events.output = [this](const QByteArray& bytes) { output += bytes; };
        events.finished = [this](const session::CommandExit& exit) { exits.append(exit); };
        auto made = std::make_unique<ExecutorCommandRunner>(options, executor, policy, events);
        made->probe();
        return made;
    }

    bool ready() {
        return QTest::qWaitFor([this] { return runner->admit(); }, 60'000);
    }
    bool finished(const int ms = 90'000) {
        return QTest::qWaitFor([this] { return !exits.isEmpty(); }, ms);
    }

    session::LaunchRequest shell(const QString& script,
                                 const QString& jobId = QStringLiteral("job-1")) const {
        session::LaunchRequest launch;
        launch.jobId = jobId;
        launch.attempt = 1;
        launch.timeoutMs = 120'000;
        launch.invocation.shell = script;
        launch.workspace = layout.workspace;
        launch.environment = {{QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin")},
                              {QStringLiteral("STATE"), layout.state},
                              {QStringLiteral("OTHER"), layout.other},
                              {QStringLiteral("WORK"), layout.work},
                              {QStringLiteral("WS"), layout.workspace},
                              {QStringLiteral("RUNTIME"), qEnvironmentVariable("XDG_RUNTIME_DIR")}};
        return launch;
    }

    QString tmpOf(const QString& jobId) const {
        return layout.work + QStringLiteral("/jobtmp/") + executorTaskId(jobId, 1);
    }

    Layout layout;
    NativeExecutor executor{program};
    LinuxSystemHost host;
    LinuxPolicy policy{host, program};
    QByteArray output;
    QList<session::CommandExit> exits;
    std::unique_ptr<ExecutorCommandRunner> runner;
};

// One executor job started directly, with an execution the runner would never build.
struct DirectJob {
    DirectJob(Layout& layout, const QString& jobId)
        : entry(*ExecutorDirectories(layout.state + QStringLiteral("/executor")).create(jobId, 1)),
          job(executor.job(entry.path, entry.taskId, entry.lease)) {}

    std::optional<QJsonObject> run(const QJsonObject& execution) {
        QString error;
        if (!job->start(
                execution, 60'000,
                [this](const QJsonObject& event) {
                    if (event.contains(QStringLiteral("execution_fingerprint")))
                        fingerprint =
                            event.value(QStringLiteral("execution_fingerprint")).toString();
                    if (event.value(QStringLiteral("event")).toString() == QStringLiteral("done"))
                        done = event;
                },
                &error))
            return std::nullopt;
        QTest::qWaitFor([this] { return done.has_value(); }, 90'000);
        if (done)
            job->acknowledge();
        return done;
    }

    ExecutorDirectories::Entry entry;
    NativeExecutor executor{program};
    std::unique_ptr<ExecutorJob> job;
    std::optional<QJsonObject> done;
    QString fingerprint;
};

QJsonObject execution(const QString& script, const QString& cwd, const QJsonValue& mounts) {
    QJsonObject made{{QStringLiteral("shell_command"), script},
                     {QStringLiteral("cwd"), cwd},
                     {QStringLiteral("env_delta"),
                      QJsonObject{{QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin")}}}};
    if (!mounts.isUndefined())
        made.insert(QStringLiteral("mounts"), mounts);
    return made;
}

QJsonObject mountsOf(const QStringList& hidden, const QString& workspace, const QString& tmp) {
    return {{QStringLiteral("hidden"), QJsonArray::fromStringList(hidden)},
            {QStringLiteral("workspace"), workspace},
            {QStringLiteral("tmp"), tmp}};
}

// Helper mode: a runner process that starts one job and waits to be killed. Only the random part
// of the marker is on its own command line, so the job's is the only one holding it whole.
int holdJob(const QString& parent, const QString& random) {
    Linux rig(parent);
    if (!rig.ready())
        return 2;
    rig.runner->launch(rig.shell(QStringLiteral("exec sleep 4000.") + random));
    QTextStream(stdout) << rig.layout.base << Qt::endl;
    QTest::qWaitFor([] { return false; }, 600'000);
    return 3;
}

} // namespace

class RunnerLinuxJobTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        if (qEnvironmentVariable("KETPLUS_RUN_LINUX_JOB_TESTS") != QStringLiteral("1"))
            QSKIP("set KETPLUS_RUN_LINUX_JOB_TESTS=1 in a prepared Linux user session");
        QVERIFY(QFileInfo(program).isExecutable());
    }

    void policyIsReady() {
        LinuxSystemHost host;
        LinuxPolicy policy(host, program);
        std::optional<bool> available;
        QString code;
        policy.probe([&](const bool isAvailable, const QString& answered) {
            available = isAvailable;
            code = answered;
        });
        QTRY_VERIFY_WITH_TIMEOUT(available.has_value(), 30'000);
        QVERIFY2(*available, qPrintable(code));
    }

    // From inside: the state, the other job's workspace and the user runtime directory are gone,
    // the hidden directories are read-only, /tmp is the job's own; from outside: what it wrote in
    // its workspace is there, its tmp directory goes with it. Also with the work directory under
    // /tmp, and the logs hold no path or command.
    void jobSeesOnlyItsWorkspace_data() {
        QTest::addColumn<QString>("parent");
        QTest::newRow("home") << QDir::homePath();
        QTest::newRow("under /tmp") << QStringLiteral("/tmp");
    }
    void jobSeesOnlyItsWorkspace() {
        QFETCH(QString, parent);
        const QString hostMarker =
            QStringLiteral("/tmp/ketplus-host-") + QUuid::createUuid().toString(QUuid::Id128);
        QVERIFY(writeFile(hostMarker, "host"));
        QLoggingCategory::setFilterRules(QStringLiteral("ketplus.runner.exec*=true"));
        logged.clear();
        const QtMessageHandler previous = qInstallMessageHandler(capture);
        Linux rig(parent);
        QVERIFY(rig.ready());
        session::LaunchRequest launch = rig.shell(QStringLiteral(
            "r=0\n"
            "for p in \"$STATE/secret\" \"$OTHER/secret\" \"$RUNTIME/bus\" \"$HOSTMARK\"; do\n"
            "  if [ -e \"$p\" ]; then echo \"visible $p\"; r=1; fi\n"
            "done\n"
            "[ \"$(pwd)\" = \"$WS/sub\" ] || { echo cwd; r=1; }\n"
            "echo inside > \"$WS/written\" || { echo workspace; r=1; }\n"
            "echo t > /tmp/job-file || { echo tmp; r=1; }\n"
            "if [ -n \"$EXPECT_RO\" ] && touch \"$STATE/new\" 2>/dev/null; then echo rw; r=1; fi\n"
            "touch \"$STATE/new\" \"$WORK/new\" 2>/dev/null\n"
            "echo checked; exit $r"));
        launch.cwd = QStringLiteral("sub");
        launch.environment.insert(QStringLiteral("HOSTMARK"), hostMarker);
        if (!parent.startsWith(QStringLiteral("/tmp")))
            launch.environment.insert(QStringLiteral("EXPECT_RO"), QStringLiteral("1"));
        rig.runner->launch(launch);
        QVERIFY(rig.finished());
        qInstallMessageHandler(previous);
        QVERIFY2(rig.exits.first().exitCode == 0, rig.output.constData());
        QVERIFY(rig.output.contains("checked"));
        QFile written(rig.layout.workspace + QStringLiteral("/written"));
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(written.readAll(), QByteArray("inside\n"));
        QVERIFY(QFileInfo(rig.layout.state + QStringLiteral("/secret")).isFile());
        QVERIFY(!QFileInfo::exists(rig.layout.state + QStringLiteral("/new")));
        QVERIFY(!QFileInfo::exists(rig.layout.work + QStringLiteral("/new")));
        QVERIFY(!QFileInfo::exists(QStringLiteral("/tmp/job-file")));
        QVERIFY(!QFileInfo::exists(rig.tmpOf(QStringLiteral("job-1"))));
        QFile::remove(hostMarker);
        for (const QString& message : logged)
            QVERIFY2(!message.contains(rig.layout.base) &&
                         !message.contains(QStringLiteral("$STATE")),
                     qPrintable(message));
    }

    // Root in its namespaces, the command cannot unmount or remount what hides the runner, not
    // even from a user and mount namespace of its own.
    void hiddenCannotBeUncovered() {
        Linux rig;
        QVERIFY(rig.ready());
        rig.runner->launch(rig.shell(QStringLiteral(
            "command -v unshare >/dev/null && command -v umount >/dev/null || exit 3\n"
            "echo \"uid_map $(cat /proc/self/uid_map)\"; grep CapEff /proc/self/status\n"
            "r=0\n"
            "if umount \"$STATE\" 2>/dev/null; then echo umount; r=1; fi\n"
            "if umount -l \"$STATE\" 2>/dev/null; then echo umount-lazy; r=1; fi\n"
            "if mount -o remount,rw \"$STATE\" 2>/dev/null; then echo remount; r=1; fi\n"
            "nested=$(unshare -Urm sh -c 'echo inner; umount \"$1\" 2>/dev/null && echo uncovered;"
            " umount -l \"$1\" 2>/dev/null && echo uncovered; ls -A \"$1\"' sh \"$STATE\")\n"
            "echo \"$nested\"\n"
            "case \"$nested\" in *inner*) ;; *) echo no-nested; r=1;; esac\n"
            "case \"$nested\" in *uncovered*|*secret*) r=1;; esac\n"
            "[ -e \"$STATE/secret\" ] && { echo visible; r=1; }\n"
            "echo checked; exit $r")));
        QVERIFY(rig.finished());
        qInfo().noquote() << "inside:" << rig.output;
        QVERIFY2(rig.exits.first().exitCode == 0, rig.output.constData());
    }

    // While a job runs, the runner outside still reads its state: no mount reached the host.
    void hostStillReadsItsState() {
        Linux rig;
        QVERIFY(rig.ready());
        rig.runner->launch(rig.shell(QStringLiteral("touch \"$WS/started\"; exec sleep 120")));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(rig.layout.workspace + "/started"), 30'000);
        QFile secret(rig.layout.state + QStringLiteral("/secret"));
        QVERIFY(secret.open(QIODevice::ReadOnly));
        QCOMPARE(secret.readAll(), QByteArray("state"));
        QFile mounts(QStringLiteral("/proc/self/mountinfo"));
        QVERIFY(mounts.open(QIODevice::ReadOnly));
        QVERIFY(!mounts.readAll().contains(rig.layout.base.toUtf8()));
        rig.runner->kill();
        QVERIFY(rig.finished());
    }

    // Mounts the launcher refuses, or that fail half way, run nothing: exit 125.
    void refusedMountsRunNothing_data() {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"nested hidden", "hidden is the workspace", "tmp in workspace",
                                 "symlink workspace", "symlink hidden", "hidden holds /tmp",
                                 "cwd outside", "no hidden", "unknown key", "fails half way"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void refusedMountsRunNothing() {
        QFETCH(QString, kind);
        Layout layout(QDir::homePath());
        const QString tmp = layout.work + QStringLiteral("/jobtmp/t");
        QDir().mkpath(tmp);
        QString workspace = layout.workspace;
        QString cwd = workspace;
        QJsonObject mounts = mountsOf({layout.state, layout.work}, workspace, tmp);
        if (kind == "nested hidden") {
            mounts = mountsOf({layout.work, layout.work + u'/' + key('a')}, workspace, tmp);
        } else if (kind == "hidden is the workspace") {
            mounts = mountsOf({layout.state, workspace}, workspace, tmp);
        } else if (kind == "tmp in workspace") {
            QDir().mkpath(workspace + QStringLiteral("/tmp"));
            mounts = mountsOf({layout.state, layout.work}, workspace, workspace + "/tmp");
        } else if (kind == "symlink workspace") {
            QFile::link(workspace, layout.base + QStringLiteral("/link"));
            mounts = mountsOf({layout.state, layout.work}, layout.base + "/link", tmp);
        } else if (kind == "symlink hidden") {
            QFile::link(layout.state, layout.base + QStringLiteral("/state-link"));
            mounts = mountsOf({layout.base + "/state-link", layout.work}, workspace, tmp);
        } else if (kind == "hidden holds /tmp") {
            mounts = mountsOf({layout.state, layout.work, QStringLiteral("/tmp")}, workspace, tmp);
        } else if (kind == "cwd outside") {
            cwd = layout.other;
        } else if (kind == "no hidden") {
            mounts = mountsOf({}, workspace, tmp);
        } else if (kind == "unknown key") {
            mounts.insert(QStringLiteral("extra"), true);
        } else if (kind == "fails half way") {
            // Deeper than the covering file system holds inodes: it is mounted, then the
            // workspace's mount point cannot be made.
            QString deep = layout.work;
            for (int level = 0; level < 140; ++level)
                deep += QStringLiteral("/d");
            QVERIFY(QDir().mkpath(deep));
            workspace = cwd = deep;
            mounts = mountsOf({layout.state, layout.work}, workspace, tmp);
        }
        DirectJob direct(layout, QStringLiteral("refused"));
        const auto done = direct.run(execution(QStringLiteral("touch ran"), cwd, mounts));
        QVERIFY(done);
        QCOMPARE(done->value(QStringLiteral("exit_code")).toInt(), 125);
        QVERIFY(!QFileInfo::exists(cwd + QStringLiteral("/ran")));
        QVERIFY(!QFileInfo::exists(workspace + QStringLiteral("/ran")));
    }

    // The executor fingerprints the whole execution: the same one twice is the same payload, other
    // mounts are another.
    void fingerprintFollowsTheMounts() {
        Layout layout(QDir::homePath());
        const QString tmp = layout.work + QStringLiteral("/jobtmp/t");
        QDir().mkpath(tmp);
        const QJsonObject first =
            execution(QStringLiteral("true"), layout.workspace,
                      mountsOf({layout.state, layout.work}, layout.workspace, tmp));
        QJsonObject other = first;
        other.insert(QStringLiteral("mounts"),
                     mountsOf({layout.work, layout.state}, layout.workspace, tmp));
        DirectJob a(layout, QStringLiteral("a"));
        DirectJob b(layout, QStringLiteral("b"));
        DirectJob c(layout, QStringLiteral("c"));
        QVERIFY(a.run(first) && b.run(first) && c.run(other));
        QVERIFY(!a.fingerprint.isEmpty());
        QCOMPARE(b.fingerprint, a.fingerprint);
        QVERIFY(c.fingerprint != a.fingerprint);
        QCOMPARE(a.done->value(QStringLiteral("exit_code")).toInt(), 0);
    }

    // Stop ends the whole tree, also what left the session or was forked twice, within the
    // executor's limits, and the cleanup is confirmed.
    void stopEndsTheTree() {
        Linux rig;
        QVERIFY(rig.ready());
        const QByteArray nonce =
            QByteArray::number(QRandomGenerator::global()->bounded(100000, 999999));
        const QString n = QString::fromLatin1(nonce);
        rig.runner->launch(rig.shell("setsid sh -c 'exec sleep 3001." + n +
                                     "' &\n"
                                     "sh -c '(exec sleep 3002." +
                                     n +
                                     ") &'\n"
                                     "sh -c 'setsid sh -c \"exec sleep 3003." +
                                     n +
                                     "\" &'\n"
                                     "echo ready; exec sleep 3000." +
                                     n));
        QTRY_COMPARE_WITH_TIMEOUT(processesWith("." + nonce), 4, 30'000);
        QElapsedTimer clock;
        clock.start();
        rig.runner->kill();
        QVERIFY(rig.finished(60'000));
        QVERIFY(clock.elapsed() < 60'000);
        QCOMPARE(rig.exits.first().cleanupStatus, QStringLiteral("verified"));
        QCOMPARE(processesWith("." + nonce), 0);
        QVERIFY(!QFileInfo::exists(rig.tmpOf(QStringLiteral("job-1"))));
        QVERIFY(rig.ready());
    }

    // The runner process is killed while its job runs; the next one stops the job, never runs it
    // again, and removes its directories.
    void killedRunnerIsCleanedUpAtStart() {
        const QByteArray random =
            QByteArray::number(QRandomGenerator::global()->bounded(100000, 999999));
        const QByteArray nonce = "4000." + random;
        QProcess holder;
        holder.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--hold-job"), QDir::homePath(), QString::fromLatin1(random)});
        QVERIFY(holder.waitForReadyRead(90'000));
        const QString base = QString::fromUtf8(holder.readLine().trimmed());
        QVERIFY(base.startsWith(QDir::homePath()));
        QTRY_COMPARE_WITH_TIMEOUT(processesWith(nonce), 1, 30'000);
        holder.kill(); // SIGKILL
        QVERIFY(holder.waitForFinished(10'000));
        QCOMPARE(processesWith(nonce), 1); // the job outlives its runner

        ExecutorCommandRunner::Options options;
        options.directory = base + QStringLiteral("/state/executor");
        options.mounts = ExecutorCommandRunner::Mounts{base + "/state", base + "/work", {}};
        NativeExecutor executor(program);
        LinuxSystemHost host;
        LinuxPolicy policy(host, program);
        int finished = 0;
        ExecutorCommandRunner::Events events;
        events.finished = [&finished](const session::CommandExit&) { ++finished; };
        ExecutorCommandRunner runner(options, executor, policy, events);
        runner.probe();
        std::optional<bool> clean;
        runner.cleanUpLeftovers([&clean](const bool isClean, const QString&) { clean = isClean; });
        QTRY_VERIFY_WITH_TIMEOUT(clean.has_value(), 90'000);
        QVERIFY(*clean);
        QCOMPARE(processesWith(nonce), 0);
        QVERIFY(ExecutorDirectories(options.directory).names().isEmpty());
        QVERIFY(QDir(base + QStringLiteral("/work/jobtmp")).isEmpty());
        QCOMPARE(finished, 0); // a left job is never reported as this runner's command
        QTRY_VERIFY_WITH_TIMEOUT(runner.admit(), 60'000);
        QDir(base).removeRecursively();
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 4 && QByteArray(argv[1]) == "--hold-job")
        return holdJob(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]));
    RunnerLinuxJobTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "RunnerLinuxJobTest.moc"
