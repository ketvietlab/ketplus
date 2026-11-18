#include "RunnerExecTest.h"

#include "exec/ExecutionPlan.h"
#include "exec/ExecutorDirectories.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QTest>

namespace runner_exec_test {
namespace {

using State = Readiness::State;
constexpr int fullPage = 8 * 1024;

bool rootEmpty(const Fixture& fixture) {
    return QDir(fixture.executorRoot()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot);
}

QStringList captured;
void capture(QtMsgType, const QMessageLogContext&, const QString& message) {
    captured.append(message);
}

} // namespace

void RunnerExecRunnerTest::readinessFollowsThePolicy() {
    Fixture f;
    QCOMPARE(f.runner->readiness().state, State::Unknown);
    QVERIFY(!f.runner->admit());
    f.runner->probe();
    f.runner->probe(); // one probe at a time
    QCOMPARE(f.policy.probes, 1);
    f.policy.answer(true);
    QCOMPARE(f.runner->readiness().state, State::Ready);
    QVERIFY(f.runner->admit());
    QCOMPARE(f.readiness.size(), 1);

    // Unavailable: probed again until it is not.
    f.policy.autoAnswer = false;
    f.policy.autoCode = QStringLiteral("cgroup_missing");
    f.runner->probe();
    QCOMPARE(f.runner->readiness(), (Readiness{State::Unavailable, "cgroup_missing"}));
    QVERIFY(!f.runner->admit());
    QTRY_VERIFY(f.policy.probes >= 4);
    QCOMPARE(f.readiness.size(), 2); // a change is told once
    f.policy.autoAnswer = true;
    QTRY_COMPARE(f.runner->readiness().state, State::Ready);
    const int probes = f.policy.probes;
    QTest::qWait(150);
    QCOMPARE(f.policy.probes, probes);
}

void RunnerExecRunnerTest::closedPolicyNeverAdmits() {
    Fixture f;
    ClosedPolicy closed;
    QList<session::CommandExit> exits;
    ExecutorCommandRunner::Options options;
    options.directory = f.executorRoot();
    ExecutorCommandRunner runner(
        options, f.executor, closed,
        {{}, {}, [&exits](const session::CommandExit& exit) { exits.append(exit); }, {}});
    runner.probe();
    QCOMPARE(runner.readiness(), (Readiness{State::Unavailable, "containment_unavailable"}));
    QVERIFY(!runner.admit());
    runner.launch(f.request());
    QCOMPARE(exits.size(), 1);
    QCOMPARE(exits.first().state, QStringLiteral("failed"));
    QCOMPARE(exits.first().reason, QStringLiteral("containment_unavailable"));
    QVERIFY(f.executor.jobs.isEmpty());
}

void RunnerExecRunnerTest::launchFailuresReportOnce() {
    Fixture f;
    f.runner->launch(f.request()); // not probed yet
    QCOMPARE(f.events, QStringList{"finished failed containment_unavailable verified"});

    f.ready();
    f.events.clear();
    session::LaunchRequest missing = f.request();
    missing.cwd = QStringLiteral("absent");
    f.runner->launch(missing);
    QCOMPARE(f.events, QStringList{"finished failed cwd_missing verified"});
    QVERIFY(f.executor.jobs.isEmpty());

    f.events.clear();
    f.executor.startOk = false;
    f.runner->launch(f.request());
    QCOMPARE(f.events, QStringList{"finished failed spawn_failed verified"});
    QVERIFY(rootEmpty(f));

    f.events.clear();
    f.executor.startOk = true;
    QVERIFY(ExecutorDirectories(f.executorRoot()).create(QStringLiteral("job-2"), 1));
    f.runner->launch(f.request(QStringLiteral("job-2")));
    QCOMPARE(f.events, QStringList{"finished failed executor_directory_unavailable verified"});
    QCOMPARE(f.runner->readiness().state, State::Ready);
}

void RunnerExecRunnerTest::launchWhileBusyIsRefused() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    f.runner->launch(f.request(QStringLiteral("job-2")));
    QCOMPARE(f.executor.jobs.size(), 1);
    QVERIFY(f.exits.isEmpty());
    QVERIFY(f.job()->stops.isEmpty());
    f.job()->done(result("completed", 0));
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.exits.first().exitCode, 0);
}

void RunnerExecRunnerTest::launchPassesTheBackstop() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.backstopMs = 60'000; });
    f.ready();
    session::LaunchRequest launch = f.request(QStringLiteral("team/job.1"));
    launch.attempt = 2;
    f.runner->launch(launch);
    const auto job = f.job();
    QCOMPARE(job->timeoutMs, 61'000);
    QCOMPARE(job->taskId, executorTaskId(launch.jobId, 2));
    QCOMPARE(job->directory, f.executorRoot() + u'/' + job->taskId);
    QVERIFY(QFileInfo::exists(job->directory + QStringLiteral("/identity.json")));
    const auto entry = ExecutorDirectories(f.executorRoot()).read(job->taskId);
    QVERIFY(entry);
    QCOMPARE(entry->jobId, launch.jobId);
    QCOMPARE(entry->lease, job->lease);
    QCOMPARE(job->execution.value("argv").toArray(), QJsonArray{QStringLiteral("make")});
    QCOMPARE(job->execution.value("cwd").toString(), f.workspace);
    const QJsonObject delta = job->execution.value("env_delta").toObject();
    QCOMPARE(delta.value("JOB").toString(), QStringLiteral("1"));
    QVERIFY(delta.value("RUNNER_ONLY").isNull());
    QCOMPARE(delta.size(), 2);
}

// 32 pages per tick; a tick that used its whole quota is followed by the next at once.
void RunnerExecRunnerTest::outputIsPaced() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.pollMs = 300; });
    f.ready();
    for (int i = 0; i < 40; ++i)
        f.executor.nextPages.append(page(QByteArray(fullPage, 'a')));
    f.runner->launch(f.request());
    const auto spool = f.executor.spools.last();
    QTRY_VERIFY_WITH_TIMEOUT(spool->readAt.size() >= 42, 3000);
    const QList<qint64>& at = spool->readAt;
    QVERIFY2(at[0] >= 250, qPrintable(QString::number(at[0])));
    QVERIFY(at[31] - at[0] < 100);
    QVERIFY(at[32] - at[31] < 100); // the next tick at once
    QVERIFY(at[40] - at[32] < 100); // the empty read ends it
    QVERIFY2(at[41] - at[40] >= 250, qPrintable(QString::number(at[41] - at[40])));
    QCOMPARE(f.output.size(), 40 * fullPage);
}

void RunnerExecRunnerTest::outputDrainsBeforeTheEnd() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.pollMs = 60'000; });
    f.ready();
    f.runner->launch(f.request());
    const auto job = f.job();
    for (int i = 0; i < 3; ++i)
        f.executor.spools.last()->pages.append(page(QByteArray(fullPage, 'a')));
    f.executor.spools.last()->pages.append(page("tail"));
    job->done(result("completed", 0));
    QVERIFY(f.events.isEmpty()); // never inside the executor's event
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.events, QStringList({"output 8192", "output 8192", "output 8192", "output 4",
                                    "finished exited  verified"}));
    QCOMPARE(job->acknowledged, 1);
    QVERIFY(rootEmpty(f));
    QTRY_VERIFY(!job->alive);
    QCOMPARE(f.runner->readiness().state, State::Ready);
}

void RunnerExecRunnerTest::lostOutputIsReported() {
    Fixture f;
    f.ready();
    f.executor.nextPages = {page("ab"), page("cd", 50)};
    f.runner->launch(f.request());
    QTRY_COMPARE(f.events.size(), 3);
    QCOMPARE(f.events, QStringList({"output 2", "lost 50", "output 2"}));
    f.job()->done(result("completed", 0));
    QTRY_COMPARE(f.exits.size(), 1);
}

void RunnerExecRunnerTest::readFailuresAreRetried() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.pollMs = 1; });
    f.ready();
    OutputSpool::Page failed;
    failed.failed = true;
    f.executor.nextPages = {failed, failed, page("x")};
    f.runner->launch(f.request());
    f.job()->done(result("completed", 0));
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.events, QStringList({"output 1", "finished exited  verified"}));

    // A store that cannot be read does not hold the end back for ever.
    f.events.clear();
    for (int i = 0; i < 100; ++i)
        f.executor.nextPages.append(failed);
    f.runner->launch(f.request(QStringLiteral("job-2")));
    f.job()->done(result("completed", 0));
    QTRY_COMPARE(f.exits.size(), 2);
    QVERIFY(f.executor.spools.last()->readAt.size() < 100);
}

void RunnerExecRunnerTest::oneEndPerCommand() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    const auto job = f.job();
    job->event({{"event", "running"}});
    job->done(result("failed", 2));
    job->done(result("completed", 0));
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.exits.first().exitCode, 2);
    job->done(result("completed", 0));
    QTest::qWait(50);
    QCOMPARE(f.exits.size(), 1);
}

void RunnerExecRunnerTest::killStopsOnce() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    const auto job = f.job();
    f.runner->kill();
    f.runner->kill();
    QCOMPARE(job->stops, QStringList{"cancelled"});
    job->done(result("cancelled", -1, "not_required"));
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.events.last(), QStringLiteral("finished failed exit_status_unknown verified"));
    f.runner->kill();
    QCOMPARE(job->stops.size(), 1);
    QTest::qWait(300); // past the kill wait
    QCOMPARE(f.exits.size(), 1);
}

void RunnerExecRunnerTest::killAfterTheEndDoesNothing() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    const auto job = f.job();
    job->done(result("completed", 0));
    f.runner->kill(); // the end is known, not reported yet
    QVERIFY(job->stops.isEmpty());
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.exits.first().exitCode, 0);
}

void RunnerExecRunnerTest::killWithoutAnAnswerLeavesItUnverified() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.pollMs = 60'000; });
    f.ready();
    f.executor.nextPages = {page("tail")};
    f.runner->launch(f.request());
    const auto job = f.job();
    f.runner->kill();
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.events, QStringList({"output 4", "finished failed cleanup_unverified unverified"}));
    QCOMPARE(f.runner->readiness(), (Readiness{State::Unavailable, "cleanup_unverified"}));
    QVERIFY(!f.runner->admit());
    QVERIFY(QFileInfo::exists(job->directory));
    QCOMPARE(job->acknowledged, 0);
    f.runner->launch(f.request(QStringLiteral("job-2")));
    QCOMPARE(f.events.last(), QStringLiteral("finished failed cleanup_unverified verified"));

    // The late answer confirms the cleanup: never a second end.
    job->done(result("cancelled", -1, "verified"));
    QTRY_COMPARE(f.runner->readiness().state, State::Ready);
    QCOMPARE(job->acknowledged, 1);
    QVERIFY(rootEmpty(f));
    QCOMPARE(f.exits.size(), 2);
}

void RunnerExecRunnerTest::unverifiedEndBlocksUntilConfirmed() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    const auto job = f.job();
    job->done(result("completed", 0, "best_effort_complete"));
    QTRY_COMPARE(f.exits.size(), 1);
    QCOMPARE(f.exits.first().exitCode, 0);
    QCOMPARE(f.exits.first().cleanupStatus, QStringLiteral("unverified"));
    QCOMPARE(f.runner->readiness().code, QStringLiteral("cleanup_unverified"));
    QVERIFY(QFileInfo::exists(job->directory));
    job->done(result("completed", 0, "unverified"));
    QTest::qWait(20);
    QVERIFY(!f.runner->admit());
    job->done(result("completed", 0, "verified"));
    QTRY_VERIFY(f.runner->admit());
    QVERIFY(rootEmpty(f));
    QCOMPARE(f.exits.size(), 1);
}

// Job ids, byte counts and codes only: never the command, its environment or its paths.
void RunnerExecRunnerTest::logsHoldNoCommandData() {
    QLoggingCategory::setFilterRules(QStringLiteral("ketplus.runner.exec*=true"));
    captured.clear();
    const QtMessageHandler previous = qInstallMessageHandler(capture);
    {
        Fixture f([](ExecutorCommandRunner::Options& options) { options.killWaitMs = 20; });
        f.ready();
        session::LaunchRequest launch = f.request();
        launch.invocation.argv =
            QStringList{QStringLiteral("secret-program"), QStringLiteral("secret-argument")};
        launch.environment.insert(QStringLiteral("TOKEN"), QStringLiteral("secret-value"));
        f.executor.nextPages = {page("secret-output")};
        f.runner->launch(launch);
        f.job()->done(result("completed", 0));
        QTRY_COMPARE(f.exits.size(), 1);
        launch.jobId = QStringLiteral("job-2");
        launch.cwd = QStringLiteral("secret-missing-dir");
        f.runner->launch(launch);
        launch.cwd.clear();
        f.runner->launch(launch);
        f.runner->kill();
        QTRY_COMPARE(f.exits.size(), 3);
        for (const QString& message : std::as_const(captured)) {
            QVERIFY2(!message.contains(QStringLiteral("secret")), qPrintable(message));
            QVERIFY2(!message.contains(f.temp.path()), qPrintable(message));
            QVERIFY2(!message.contains(f.workspace), qPrintable(message));
        }
    }
    qInstallMessageHandler(previous);
    QLoggingCategory::setFilterRules({});
    QVERIFY(captured.size() >= 4);
    QVERIFY(captured.join(u'\n').contains(QStringLiteral("job-1")));
}

} // namespace runner_exec_test
