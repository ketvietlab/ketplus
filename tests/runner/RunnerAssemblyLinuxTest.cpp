#include "RunnerAssemblyRig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <signal.h>

// Real jobs through `run` as the runner's main wires it (RunnerAssembly::create), with the job
// executor next to this binary, over the fake Relay on loopback and a paired desktop the test
// plays. It needs a user session with delegated cgroup v2 and unprivileged user namespaces, so
// it runs only with KETPLUS_RUN_LINUX_JOB_TESTS=1.
namespace runner_assembly_test {
namespace {

const QString helperMode = QStringLiteral("--hold-job");

// The test's directories live beside this binary, outside /tmp: a job gets its own /tmp, and the
// trace directory it writes to must stay visible to the test.
QString rootTemplate() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/assembly-XXXXXX");
}

QByteArray sleepMarker() {
    return "4000." +
           QByteArray::number(QRandomGenerator::global()->bounded(100'000'000, 999'999'999));
}

protocol::JobSpec specFor(const QString& script, const qint64 timeoutMs) {
    protocol::JobSpec spec;
    spec.attempt = 1;
    spec.invocation.argv = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), script};
    spec.timeoutMs = timeoutMs;
    spec.repositoryKey = QString(32, QLatin1Char('r'));
    spec.workspaceKey = QString(32, QLatin1Char('w'));
    spec.objectFormat = QStringLiteral("sha1");
    return spec;
}

FakeRelay::Job queuedJob() {
    FakeRelay::Job job;
    job.id = QStringLiteral("job_1");
    return job;
}

const FakeRelay::Lease* leaseOf(const FakeRelay& relay) {
    return relay.leases.isEmpty() ? nullptr : &relay.leases.first();
}

bool emptyDirectory(const QString& path) {
    return !QFileInfo::exists(path) ||
           QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty();
}

// The configuration of the snapshot library comes from the caller; the machine's must not shape
// these tests.
void privateGitConfiguration(const QString& root) {
    QDir().mkpath(root + "/home");
    qputenv("XDG_CONFIG_HOME", (root + "/home/.config").toUtf8());
    qputenv("GIT_CONFIG_GLOBAL", (root + "/home/.gitconfig").toUtf8());
}

} // namespace

class RunnerAssemblyLinuxTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase();
    void realJobs_data();
    void realJobs();
    void killedRunnerFinishesTheJobOnceAfterRestart();
};

void RunnerAssemblyLinuxTest::initTestCase() {
    if (qEnvironmentVariable("KETPLUS_RUN_LINUX_JOB_TESTS") != QStringLiteral("1"))
        QSKIP("set KETPLUS_RUN_LINUX_JOB_TESTS=1 in a prepared Linux user session");
    QVERIFY(
        QFileInfo(QCoreApplication::applicationDirPath() + "/ketplus_job_executor").isExecutable());
}

void RunnerAssemblyLinuxTest::realJobs_data() {
    QTest::addColumn<QString>("script");
    QTest::addColumn<qint64>("timeoutMs");
    QTest::addColumn<QString>("state");
    QTest::addColumn<qint64>("exitCode");
    QTest::addColumn<QString>("outcome");
    QTest::addColumn<QString>("reason");
    QTest::newRow("exit 0") << QStringLiteral("echo \"out-$MARK\"; exit 0") << qint64(600'000)
                            << QStringLiteral("exited") << qint64(0) << QStringLiteral("succeeded")
                            << QStringLiteral("exited");
    QTest::newRow("exit 3") << QStringLiteral("echo \"out-$MARK\"; exit 3") << qint64(600'000)
                            << QStringLiteral("exited") << qint64(3) << QStringLiteral("failed")
                            << QStringLiteral("exited_nonzero");
    QTest::newRow("timed out") << QStringLiteral("echo \"out-$MARK\"; exec sleep MARKER")
                               << qint64(3'000) << QStringLiteral("timed_out") << qint64(-1)
                               << QStringLiteral("failed") << QStringLiteral("timed_out");
}

// One job each through the production wiring: its output, its end at the Relay, and nothing of
// it left on the machine.
void RunnerAssemblyLinuxTest::realJobs() {
    QFETCH(QString, script);
    QFETCH(qint64, timeoutMs);
    QFETCH(QString, state);
    QFETCH(qint64, exitCode);
    QFETCH(QString, outcome);
    QFETCH(QString, reason);

    QTemporaryDir root(rootTemplate());
    QVERIFY(root.isValid());
    privateGitConfiguration(root.path());
    FakeRelay relay([] { return QDateTime::currentDateTimeUtc(); });
    relay.jobs.append(queuedJob());
    Rig rig(root.path(), relay.url());
    QVERIFY(rig.registerRunner(relay));
    QVERIFY(rig.start());
    const QByteArray marker = sleepMarker();
    script.replace(QStringLiteral("MARKER"), QString::fromLatin1(marker));
    protocol::JobSpec spec = specFor(script, timeoutMs);
    spec.env = {{QStringLiteral("MARK"), QStringLiteral("linux")}};
    const QString repo = rig.repository({{"src/main.c", "int main(void) { return 0; }\n"}});
    QVERIFY(!repo.isEmpty());
    QVERIFY2(
        rig.runDesktop(desktopOf(relay), repo, spec),
        qPrintable(rig.failedStep() + QStringLiteral(": ") + rig.lines().join(QLatin1Char('\n'))));

    QVERIFY(QTest::qWaitFor([&] { return rig.result().has_value(); }, 90'000));
    const protocol::JobResult result = *rig.result();
    QCOMPARE(result.state, state);
    QCOMPARE(result.cleanupStatus, QStringLiteral("verified"));
    if (exitCode >= 0)
        QCOMPARE(result.exitCode.value_or(-1), exitCode);
    QCOMPARE(rig.output(), QByteArray("out-linux\n"));
    QCOMPARE(processesWith(marker), 0);

    rig.acknowledge(result.jobId);
    QVERIFY(QTest::qWaitFor(
        [&] { return leaseOf(relay) && !leaseOf(relay)->recordedOutcome.isEmpty(); }, 30'000));
    QCOMPARE(leaseOf(relay)->recordedOutcome, outcome);
    QCOMPARE(leaseOf(relay)->recordedReason, reason);
    QCOMPARE(relay.count("POST", "/finish"), 1);
    QVERIFY(rig.stop(60'000));
    QVERIFY(emptyDirectory(rig.stateDirectory() + "/executor"));
    QVERIFY(emptyDirectory(rig.workDirectory() + "/jobtmp"));
}

// The runner killed while its job runs, started again on the same state directory: the job's
// processes go, the Relay hears the job end as interrupted exactly once, and the command never
// runs a second time.
void RunnerAssemblyLinuxTest::killedRunnerFinishesTheJobOnceAfterRestart() {
    QTemporaryDir root(rootTemplate());
    QVERIFY(root.isValid());
    privateGitConfiguration(root.path());
    const QString trace = root.path() + "/trace";
    QVERIFY(QDir().mkpath(trace));
    FakeRelay relay([] { return QDateTime::currentDateTimeUtc(); });
    relay.jobs.append(queuedJob());
    {
        Rig registering(root.path(), relay.url());
        QVERIFY(registering.registerRunner(relay));
    }
    const Desktop desktop = desktopOf(relay);
    const QByteArray marker = sleepMarker();

    // The first runner is a process of its own, so it can be killed. It reads the marker from the
    // trace directory: no process but the job's may carry it.
    QFile markerFile(trace + "/marker");
    QVERIFY(markerFile.open(QIODevice::WriteOnly) && markerFile.write(marker) == marker.size());
    markerFile.close();
    QProcess first;
    first.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    first.start(QCoreApplication::applicationFilePath(),
                {helperMode, root.path(), relay.url().toString(), desktop.machineId,
                 desktop.workspaceId, QString::fromLatin1(desktop.signingPrivateKey.toHex()),
                 desktop.runnerId});
    QVERIFY(first.waitForStarted(10'000));
    QVERIFY2(QTest::qWaitFor(
                 [&] {
                     return first.readAllStandardOutput().contains("holding") ||
                            first.state() == QProcess::NotRunning;
                 },
                 120'000) &&
                 first.state() == QProcess::Running,
             "the first runner did not start the job");
    QVERIFY2(processesWith(marker) == 1, qPrintable(processesHolding(marker).join(u'\n')));
    ::kill(pid_t(first.processId()), SIGKILL);
    QVERIFY(first.waitForFinished(10'000));
    // A killed runner leaves its job behind.
    QVERIFY2(processesWith(marker) == 1, qPrintable(processesHolding(marker).join(u'\n')));
    QCOMPARE(relay.count("POST", "/finish"), 0);

    Rig again(root.path(), relay.url());
    QVERIFY(again.start());
    QVERIFY2(
        QTest::qWaitFor(
            [&] { return leaseOf(relay) && !leaseOf(relay)->recordedOutcome.isEmpty(); }, 150'000),
        qPrintable(again.lines().join(QLatin1Char('\n'))));
    QCOMPARE(leaseOf(relay)->recordedOutcome, QStringLiteral("interrupted"));
    QCOMPARE(leaseOf(relay)->recordedReason, QStringLiteral("runner_restarted"));
    QVERIFY(QTest::qWaitFor([&] { return processesWith(marker) == 0; }, 30'000));
    // Nothing more comes: no second end and no second run.
    QTest::qWait(3'000);
    QCOMPARE(relay.count("POST", "/finish"), 1);
    QFile runs(trace + "/runs");
    QVERIFY(runs.open(QIODevice::ReadOnly));
    QCOMPARE(runs.readAll(), QByteArray("run\n"));
    QVERIFY(again.stop(60'000));
    QVERIFY(emptyDirectory(again.stateDirectory() + "/executor"));
    QVERIFY(emptyDirectory(again.workDirectory() + "/jobtmp"));
}

// Helper mode: the first runner of the restart test. It runs the job up to its command, says
// "holding" and waits to be killed.
int holdJob(const QStringList& arguments) {
    if (arguments.size() != 7)
        return 2;
    const QString root = arguments.at(1);
    const Desktop desktop{arguments.at(3), arguments.at(4), 1,
                          QByteArray::fromHex(arguments.at(5).toLatin1()), arguments.at(6)};
    const QString trace = root + "/trace";
    QFile markerFile(trace + "/marker");
    if (!markerFile.open(QIODevice::ReadOnly))
        return 2;
    const QString marker = QString::fromLatin1(markerFile.readAll());
    Rig rig(root, QUrl(arguments.at(2)));
    if (!rig.start())
        return 3;
    protocol::JobSpec spec =
        specFor(QStringLiteral("printf 'run\\n' >> \"$TRACE/runs\"; : > \"$TRACE/started\"; "
                               "exec sleep ") +
                    marker,
                600'000);
    spec.env = {{QStringLiteral("TRACE"), trace}};
    const QString repo = rig.repository({{"a.txt", "a\n"}});
    if (repo.isEmpty() || !rig.runDesktop(desktop, repo, spec))
        return 4;
    if (!QTest::qWaitFor([&] { return QFileInfo::exists(trace + "/started"); }, 90'000))
        return 5;
    QTextStream(stdout) << "holding" << Qt::endl;
    (void)QTest::qWaitFor([] { return false; }, 600'000);
    return 6;
}

} // namespace runner_assembly_test

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments().mid(1);
    if (!arguments.isEmpty() && arguments.first() == runner_assembly_test::helperMode)
        return runner_assembly_test::holdJob(arguments);
    runner_assembly_test::RunnerAssemblyLinuxTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "RunnerAssemblyLinuxTest.moc"
