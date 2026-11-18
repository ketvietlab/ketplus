#include "RunnerRunTest.h"

#include "exec/LinuxPolicy.h"
#include "exec/LinuxSystemHost.h"
#include "exec/NativeExecutor.h"
#include "run/ExecutorCommandHost.h"

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTest>

// Jobs through the run loop with the production command runner on Linux: the real job executor,
// the Linux policy and the job's mounts, over the fake Relay and desktop. It needs a user session
// with delegated cgroup v2 and unprivileged user namespaces, so it runs only with
// KETPLUS_RUN_LINUX_JOB_TESTS=1.
namespace runner_run_test {
namespace {

const QString program = QStringLiteral(KETPLUS_JOB_EXECUTOR_PATH);

// Waits for `done`, settling the loop's Relay calls on the way.
bool settleUntil(Run& run, const std::function<bool()>& done, const int ms = 90'000) {
    return QTest::qWaitFor(
        [&] {
            run.settle();
            return done();
        },
        ms);
}

const protocol::JobResult* resultOf(const QList<protocol::Frame>& frames) {
    for (const protocol::Frame& frame : frames)
        if (const auto* result = std::get_if<protocol::JobResult>(&frame))
            return result;
    return nullptr;
}

} // namespace

void RunnerRunLinuxTest::initTestCase() {
    if (qEnvironmentVariable("KETPLUS_RUN_LINUX_JOB_TESTS") != QStringLiteral("1"))
        QSKIP("set KETPLUS_RUN_LINUX_JOB_TESTS=1 in a prepared Linux user session");
    QVERIFY(QFileInfo(program).isExecutable());
}

void RunnerRunLinuxTest::realJobs_data() {
    QTest::addColumn<QString>("script");
    QTest::addColumn<bool>("timesOut");
    QTest::addColumn<QString>("finish");
    QTest::addColumn<qint64>("exitCode");
    QTest::newRow("exit 0") << QStringLiteral("echo \"out-$MARK\"; exit 0") << false
                            << QStringLiteral("finish succeeded exited") << qint64(0);
    QTest::newRow("exit 3") << QStringLiteral("echo \"out-$MARK\"; exit 3") << false
                            << QStringLiteral("finish failed exited_nonzero") << qint64(3);
    QTest::newRow("timed out") << QStringLiteral(
                                      "echo \"out-$MARK\"; touch started; exec sleep 600")
                               << true << QStringLiteral("finish failed timed_out") << qint64(-1);
}

void RunnerRunLinuxTest::realJobs() {
    QFETCH(QString, script);
    QFETCH(bool, timesOut);
    QFETCH(QString, finish);
    QFETCH(qint64, exitCode);

    Run run;
    const QString base = QFileInfo(run.root.path()).canonicalFilePath();
    const QString work = base + QStringLiteral("/work");
    const protocol::JobSpec model = makeSpec();
    const QString workspace = work + u'/' + model.repositoryKey + u'/' + model.workspaceKey;
    QVERIFY(QDir().mkpath(workspace));
    QVERIFY(QFile::setPermissions(work, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    exec::ExecutorCommandRunner::Options options;
    options.directory = run.state->path() + QStringLiteral("/executor");
    options.mounts = exec::ExecutorCommandRunner::Mounts{run.state->path(), work, runtime};
    exec::NativeExecutor executor(program);
    exec::LinuxSystemHost system(runtime);
    exec::LinuxPolicy policy(system, program);
    app::ExecutorCommandHost host(options, executor, policy);
    run.realCommand = &host;
    const auto loopFirst = qScopeGuard([&run] { run.loop.reset(); }); // it holds the host

    run.start();
    run.socketReady();
    QVERIFY(settleUntil(
        run, [&host] { return host.readiness() == CommandHost::Readiness::Ready; }, 60'000));
    run.acquire();
    run.connectDesktop();
    protocol::JobSpec spec = makeSpec();
    spec.invocation.argv = QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), script};
    spec.env = {{QStringLiteral("MARK"), QStringLiteral("linux")}};
    run.deliver(spec);
    QCOMPARE(run.receiver.begun.size(), 1);
    run.receiver.ready(workspace, {});
    run.settle();
    QVERIFY(run.sentTypes().contains(QStringLiteral("job.started")));
    if (timesOut) {
        QVERIFY(settleUntil(run, [&] { return QFileInfo::exists(workspace + "/started"); }));
        run.advance(spec.timeoutMs);
    }
    QVERIFY(settleUntil(run, [&run] { return resultOf(run.sentFrames()) != nullptr; }));
    const protocol::JobResult result = *resultOf(run.sentFrames());
    QCOMPARE(result.cleanupStatus, QStringLiteral("verified"));
    if (exitCode >= 0)
        QCOMPARE(result.exitCode.value_or(-1), exitCode);
    else
        QCOMPARE(result.state, QStringLiteral("timed_out"));
    QByteArray output;
    for (const protocol::Frame& frame : run.sentFrames())
        if (const auto* chunk = std::get_if<protocol::JobOutput>(&frame))
            output += chunk->data;
    QCOMPARE(output, QByteArray("out-linux\n"));

    run.deliver(protocol::JobResultAck{7, spec.jobId});
    QVERIFY2(settleUntil(
                 run, [&] { return run.leaseKinds().contains(finish); }, 10'000),
             qPrintable(run.leaseKinds().join(u',')));
    QStringList kinds = run.leaseKinds();
    kinds.removeAll(QStringLiteral("renew")); // the time moved on renews the lease
    QCOMPARE(kinds, (QStringList{"acquire", "start", finish}));
    QVERIFY(settleUntil(run, [&host] { return host.runner().admit(); }, 30'000));
    QVERIFY(QDir(options.directory).isEmpty());
    QVERIFY(QDir(work + QStringLiteral("/jobtmp")).isEmpty());
    run.advance(heartbeatSpacingMs);
    QCOMPARE(run.heartbeatState(), QStringLiteral("idle"));
}

} // namespace runner_run_test
