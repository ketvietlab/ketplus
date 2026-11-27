#include "exec/ExecutorCommandRunner.h"
#include "exec/ExecutorDirectories.h"
#include "exec/NativeExecutor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

#include <csignal>
#include <memory>
#include <optional>

#include <unistd.h>

// The command runner over the real job executor of this build. The policy that lets it run
// exists only here: the runner's own is closed on every platform.
using namespace ketplus::runner;
using namespace ketplus::runner::exec;

namespace {

class OpenPolicy final : public PlatformPolicy {
  public:
    void probe(std::function<void(bool, const QString&)> done) override { done(true, {}); }
};

struct Native {
    Native() {
        root = QFileInfo(temp.path()).canonicalFilePath();
        workspace = root + QStringLiteral("/ws");
        QDir().mkpath(workspace + QStringLiteral("/sub"));
        runner = make();
    }

    std::unique_ptr<ExecutorCommandRunner> make() {
        ExecutorCommandRunner::Options options;
        options.directory = root + QStringLiteral("/state/executor");
        ExecutorCommandRunner::Events events;
        events.output = [this](const QByteArray& bytes) { output += bytes; };
        events.outputLost = [this](const qint64 bytes) { lost += bytes; };
        events.finished = [this](const session::CommandExit& exit) { exits.append(exit); };
        auto made = std::make_unique<ExecutorCommandRunner>(options, executor, policy, events);
        made->probe();
        return made;
    }

    session::LaunchRequest shell(const QString& script, const QString& jobId = "job-1") const {
        session::LaunchRequest launch;
        launch.jobId = jobId;
        launch.attempt = 1;
        launch.timeoutMs = 120'000;
        launch.invocation.shell = script;
        launch.workspace = workspace;
        launch.environment = {{QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin")},
                              {QStringLiteral("JOB"), QStringLiteral("job value")}};
        return launch;
    }

    bool finished(const int ms = 60'000) {
        return QTest::qWaitFor([this] { return !exits.isEmpty(); }, ms);
    }
    qint64 pidIn(const QString& name) const {
        QFile file(workspace + u'/' + name);
        return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : 0;
    }

    QTemporaryDir temp{QStringLiteral("/tmp/ketplus-runner-XXXXXX")};
    QString root;
    QString workspace;
    NativeExecutor executor{QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH)};
    OpenPolicy policy;
    QByteArray output;
    qint64 lost = 0;
    QList<session::CommandExit> exits;
    std::unique_ptr<ExecutorCommandRunner> runner;
};

[[maybe_unused]] bool gone(const qint64 pid) {
    return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0;
}

#ifdef Q_OS_LINUX
// A job's pids are its own namespace's: from here its processes are found by a marker in their
// command line.
int processesWith(const QByteArray& marker) {
    int found = 0;
    for (const QString& pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs)) {
        QFile file(QStringLiteral("/proc/") + pid + QStringLiteral("/cmdline"));
        if (pid.toLongLong() > 0 && file.open(QIODevice::ReadOnly) &&
            file.readAll().contains(marker))
            ++found;
    }
    return found;
}

QByteArray nonce() {
    return "300." + QByteArray::number(::getpid()) +
           QByteArray::number(QRandomGenerator::global()->bounded(100000));
}
#endif

} // namespace

class RunnerExecNativeTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
#ifdef Q_OS_LINUX
        if (qEnvironmentVariable("KETPLUS_RUN_LINUX_JOB_TESTS") != QStringLiteral("1"))
            QSKIP("set KETPLUS_RUN_LINUX_JOB_TESTS=1 in a prepared Linux user session");
#endif
        QVERIFY(QFileInfo(QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH)).isExecutable());
    }

    // A variable of the runner's process is not inherited: only the request's environment is.
    void environmentIsExactlyTheRequests() {
        qputenv("KETPLUS_STRAY_VARIABLE", "stray");
        Native n;
        QVERIFY(n.runner->admit());
        n.runner->launch(n.shell(QStringLiteral(
            "printf '%s|%s|%s' \"${KETPLUS_STRAY_VARIABLE-unset}\" \"${HOME-unset}\" \"$JOB\"")));
        QVERIFY(n.finished());
        qunsetenv("KETPLUS_STRAY_VARIABLE");
        QCOMPARE(n.output, QByteArray("unset|unset|job value"));
        QCOMPARE(n.exits.first().exitCode, 0);
    }

    void argvRunsInTheCwd() {
        Native n;
        session::LaunchRequest launch = n.shell({});
        launch.invocation = {QStringList{QStringLiteral("/bin/pwd")}, std::nullopt};
        launch.cwd = QStringLiteral("sub");
        n.runner->launch(launch);
        QVERIFY(n.finished());
        QCOMPARE(n.output, (n.workspace + QStringLiteral("/sub\n")).toUtf8());
        QCOMPARE(n.exits.first().exitCode, 0);
    }

    void exitCodeIsReported() {
        Native n;
        n.runner->launch(n.shell(QStringLiteral("echo out; echo err >&2; exit 7")));
        QVERIFY(n.finished());
        QCOMPARE(n.exits.first().state, QStringLiteral("exited"));
        QCOMPARE(n.exits.first().exitCode, 7);
        QCOMPARE(n.output, QByteArray("out\nerr\n")); // stdout and stderr in one stream
    }

    // macOS reports no exit code for a command a signal ended; Linux reports 128 + the signal.
    void signalledCommandHasNoExitCode() {
        Native n;
#ifdef Q_OS_LINUX
        // The command is its namespace's init, which no signal of its own ends: a child is killed.
        n.runner->launch(n.shell(QStringLiteral("sh -c 'kill -KILL $$'; exit $?")));
        QVERIFY(n.finished());
        QCOMPARE(n.exits.first().exitCode, 137);
#else
        n.runner->launch(n.shell(QStringLiteral("kill -KILL $$")));
        QVERIFY(n.finished());
        QCOMPARE(n.exits.first().state, QStringLiteral("failed"));
        QCOMPARE(n.exits.first().reason, QStringLiteral("exit_status_unknown"));
        QVERIFY(!n.exits.first().exitCode);
        QVERIFY(!n.exits.first().signal);
#endif
    }

    // Many MiB of binary and UTF-8 bytes arrive whole and in order, all before the end.
    void largeOutputArrivesWhole() {
        Native n;
        QByteArray block;
        for (int value = 0; value < 256; ++value)
            block.append(static_cast<char>(value));
        block.append(QStringLiteral("Tiếng Việt ✨ \r\n").toUtf8());
        QByteArray content;
        while (content.size() < 6 * 1024 * 1024)
            content.append(block);
        QFile file(n.workspace + QStringLiteral("/data.bin"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(content), content.size());
        file.close();
        n.runner->launch(n.shell(QStringLiteral("cat data.bin")));
        QVERIFY(n.finished(120'000));
        QCOMPARE(n.lost, 0);
        QCOMPARE(n.output.size(), content.size());
        QCOMPARE(QCryptographicHash::hash(n.output, QCryptographicHash::Sha256),
                 QCryptographicHash::hash(content, QCryptographicHash::Sha256));
        QCOMPARE(n.exits.first().exitCode, 0);
    }

    // kill() goes through the executor's stop: a command and a child of its group that ignore
    // SIGTERM end too, and the end is reported once.
    void killEndsTheGroupIgnoringTerm() {
        Native n;
#ifdef Q_OS_LINUX
        const QByteArray marker = nonce();
        n.runner->launch(n.shell(QStringLiteral("trap '' TERM; sh -c 'trap \"\" TERM; sleep ") +
                                 QString::fromLatin1(marker) +
                                 QStringLiteral("' & echo ready; wait; wait")));
        QTRY_VERIFY_WITH_TIMEOUT(n.output.contains("ready") && processesWith(marker) >= 1, 20'000);
        n.runner->kill();
        QVERIFY(n.finished(70'000));
        QCOMPARE(processesWith(marker), 0);
        QTest::qWait(200);
        QCOMPARE(n.exits.size(), 1);
        QCOMPARE(n.exits.first().cleanupStatus, QStringLiteral("verified"));
        QVERIFY(n.runner->admit());
#else
        n.runner->launch(n.shell(
            QStringLiteral("trap '' TERM; sh -c 'trap \"\" TERM; echo $$ > child.pid; sleep 300' & "
                           "echo $$ > root.pid; echo ready; wait; wait")));
        QTRY_VERIFY_WITH_TIMEOUT(n.output.contains("ready") && n.pidIn("child.pid") > 0, 20'000);
        const qint64 root = n.pidIn(QStringLiteral("root.pid"));
        const qint64 child = n.pidIn(QStringLiteral("child.pid"));
        QVERIFY(!gone(root) && !gone(child));
        n.runner->kill();
        QVERIFY(n.finished(30'000));
        QTRY_VERIFY_WITH_TIMEOUT(gone(root) && gone(child), 10'000);
        QTest::qWait(200);
        QCOMPARE(n.exits.size(), 1);
        // On macOS the executor's cleanup is best effort: the job's directory stays.
        QCOMPARE(n.exits.first().cleanupStatus, QStringLiteral("unverified"));
        QVERIFY(!n.runner->admit());
#endif
    }

    // A job left by a runner that went away is stopped by the next one, never run again.
    void leftJobIsStoppedAfterARestart() {
        Native n;
#ifdef Q_OS_LINUX
        const QByteArray marker = nonce();
        n.runner->launch(n.shell(QStringLiteral("exec sleep ") + QString::fromLatin1(marker),
                                 QStringLiteral("team/job.1")));
        QTRY_COMPARE_WITH_TIMEOUT(processesWith(marker), 1, 20'000);
        n.runner.reset();
        QCOMPARE(processesWith(marker), 1);
        n.runner = n.make();
        std::optional<bool> clean;
        n.runner->cleanUpLeftovers(
            [&clean](const bool isClean, const QString&) { clean = isClean; });
        QTRY_VERIFY_WITH_TIMEOUT(clean.has_value(), 90'000);
        QVERIFY(*clean);
        QCOMPARE(processesWith(marker), 0);
        QVERIFY(ExecutorDirectories(n.root + QStringLiteral("/state/executor")).names().isEmpty());
        QVERIFY(n.runner->admit());
        QVERIFY(n.exits.isEmpty());
#else
        n.runner->launch(n.shell(QStringLiteral("echo $$ > root.pid; exec sleep 300"),
                                 QStringLiteral("team/job.1")));
        QTRY_VERIFY_WITH_TIMEOUT(n.pidIn(QStringLiteral("root.pid")) > 0, 20'000);
        const qint64 root = n.pidIn(QStringLiteral("root.pid"));
        n.runner.reset();
        QVERIFY(!gone(root));

        n.runner = n.make();
        bool called = false;
        bool clean = true;
        QString code;
        n.runner->cleanUpLeftovers([&](const bool isClean, const QString& leftCode) {
            called = true;
            clean = isClean;
            code = leftCode;
        });
        QTRY_VERIFY_WITH_TIMEOUT(called, 90'000);
        QTRY_VERIFY_WITH_TIMEOUT(gone(root), 15'000);
        // macOS cannot confirm the cleanup: the directory is kept and no command is admitted.
        QVERIFY(!clean);
        QCOMPARE(code, QStringLiteral("cleanup_unverified"));
        const ExecutorDirectories directories(n.root + QStringLiteral("/state/executor"));
        QCOMPARE(directories.names().size(), 1);
        QVERIFY(directories.read(directories.names().first()));
        QVERIFY(!n.runner->admit());
        QVERIFY(n.exits.isEmpty());
#endif
    }
};

QTEST_GUILESS_MAIN(RunnerExecNativeTest)
#include "RunnerExecNativeTest.moc"
