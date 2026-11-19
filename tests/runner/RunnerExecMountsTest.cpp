#include "RunnerExecTest.h"

#include "exec/ExecutionPlan.h"
#include "exec/ExecutorDirectories.h"
#include "exec/JobTmpDirectories.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTest>

// The command runner with mounts: each job gets its own tmp directory beside the workspaces, the
// execution carries the plan, and the tmp directory goes with the job's confirmed cleanup.
namespace runner_exec_test {
namespace {

QString base(const Fixture& f) { return QFileInfo(f.temp.path()).canonicalFilePath(); }

void withMounts(Fixture& f) {
    QDir().mkpath(base(f) + QStringLiteral("/work/repo/ws"));
    f.workspace = base(f) + QStringLiteral("/work/repo/ws");
}

// The fixture's executor directories are <base>/state/executor: the state and work directories
// sit beside each other in <base>.
void mounted(ExecutorCommandRunner::Options& options) {
    const QString real = QDir::cleanPath(options.directory + QStringLiteral("/../.."));
    QDir().mkpath(real + QStringLiteral("/state"));
    QDir().mkpath(real + QStringLiteral("/work"));
    options.mounts = ExecutorCommandRunner::Mounts{
        real + QStringLiteral("/state"), real + QStringLiteral("/work"), {}};
}

QString tmpOf(const Fixture& f, const QString& jobId) {
    return base(f) + QStringLiteral("/work/jobtmp/") + executorTaskId(jobId, 1);
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

void RunnerExecMountsTest::launchCarriesTheMounts() {
    Fixture f(mounted);
    withMounts(f);
    f.ready();
    f.runner->launch(f.request());
    QCOMPARE(f.executor.jobs.size(), 1);
    const QJsonObject mounts = f.job()->execution.value(QStringLiteral("mounts")).toObject();
    const QString real = base(f);
    QCOMPARE(mounts.value(QStringLiteral("hidden")).toArray(),
             QJsonArray({real + QStringLiteral("/state"), real + QStringLiteral("/work")}));
    QCOMPARE(mounts.value(QStringLiteral("workspace")).toString(), f.workspace);
    const QString tmp = mounts.value(QStringLiteral("tmp")).toString();
    QCOMPARE(tmp, real + QStringLiteral("/work/jobtmp/") + f.job()->taskId);
    QVERIFY(QFileInfo(tmp).isDir());
    QCOMPARE(QFileInfo(tmp).permissions() & ~(QFile::ReadUser | QFile::WriteUser | QFile::ExeUser),
             QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    QVERIFY(writeFile(tmp + QStringLiteral("/scratch"), "x"));
    f.job()->done(result("completed", 0));
    QTRY_COMPARE(f.exits.size(), 1);
    QVERIFY(!QFileInfo::exists(tmp));
}

void RunnerExecMountsTest::withoutMountsNothingIsAdded() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    QVERIFY(!f.job()->execution.contains(QStringLiteral("mounts")));
}

// A workspace the plan refuses: no supervisor starts, and neither directory stays.
void RunnerExecMountsTest::refusedPlanRunsNothing() {
    Fixture f(mounted);
    f.ready(); // the workspace stays outside the work directory
    f.runner->launch(f.request());
    QVERIFY(f.executor.jobs.isEmpty());
    QCOMPARE(f.events, QStringList{"finished failed workspace_outside_work verified"});
    QVERIFY(!QFileInfo::exists(tmpOf(f, QStringLiteral("job-1"))));
    QVERIFY(QDir(f.executorRoot()).isEmpty());
}

void RunnerExecMountsTest::failedStartRemovesTheTmp() {
    Fixture f(mounted);
    withMounts(f);
    f.executor.startOk = false;
    f.ready();
    f.runner->launch(f.request());
    QCOMPARE(f.events, QStringList{"finished failed spawn_failed verified"});
    QVERIFY(!QFileInfo::exists(tmpOf(f, QStringLiteral("job-1"))));
}

void RunnerExecMountsTest::unverifiedEndKeepsTheTmp() {
    Fixture f(mounted);
    withMounts(f);
    f.ready();
    f.runner->launch(f.request());
    const QString tmp = tmpOf(f, QStringLiteral("job-1"));
    f.job()->done(result("completed", 0, "best_effort_complete"));
    QTRY_COMPARE(f.exits.size(), 1);
    QVERIFY(QFileInfo(tmp).isDir());
    f.job()->done(result("completed", 0, "verified"));
    QTRY_VERIFY(f.runner->admit());
    QVERIFY(!QFileInfo::exists(tmp));
}

// At start: the tmp directory of a job whose directory is gone is removed whatever it holds,
// without following its links; the one of a job still left stays with it.
void RunnerExecMountsTest::leftTmpIsRemovedAtStart() {
    Fixture f(mounted);
    const QString jobs = base(f) + QStringLiteral("/work/jobtmp/");
    const QString stale = jobs + executorTaskId(QStringLiteral("gone"), 1);
    QVERIFY(QDir().mkpath(stale + QStringLiteral("/locked/deeper")));
    QVERIFY(writeFile(stale + QStringLiteral("/locked/deeper/file"), "x"));
    const QString outside = base(f) + QStringLiteral("/outside");
    QVERIFY(writeFile(outside, "keep"));
    QVERIFY(QFile::link(outside, stale + QStringLiteral("/link")));
    QVERIFY(QFile::setPermissions(stale + QStringLiteral("/locked"),
                                  QFile::ReadOwner | QFile::ExeOwner));
    QVERIFY(writeFile(jobs + QStringLiteral("stray"), "x"));
    const auto left = ExecutorDirectories(f.executorRoot()).create(QStringLiteral("left"), 1);
    QVERIFY(left);
    const QString leftTmp = jobs + left->taskId;
    QVERIFY(QDir().mkpath(leftTmp));

    f.ready();
    bool clean = true;
    f.runner->cleanUpLeftovers([&clean](const bool isClean, const QString&) { clean = isClean; });
    f.job()->done(result("interrupted", -1, "best_effort_complete"));
    QTRY_VERIFY(!clean);
    QVERIFY(!QFileInfo::exists(stale));
    QVERIFY(!QFileInfo::exists(jobs + QStringLiteral("stray")));
    QVERIFY(QFileInfo(outside).isFile());
    QVERIFY(QFileInfo(leftTmp).isDir());

    QTRY_COMPARE(f.executor.jobs.size(), 2); // retried
    f.job()->done(result("interrupted", -1, "verified"));
    QTRY_VERIFY(f.runner->admit());
    QVERIFY(!QFileInfo::exists(leftTmp));
}

void RunnerExecMountsTest::tmpOfAnEarlierAttemptIsEmptied() {
    QTemporaryDir root;
    const QString jobs = QFileInfo(root.path()).canonicalFilePath() + QStringLiteral("/jobtmp");
    JobTmpDirectories tmp(jobs);
    const QString taskId = executorTaskId(QStringLiteral("job"), 1);
    const std::optional<QString> first = tmp.create(taskId);
    QVERIFY(first);
    QVERIFY(writeFile(*first + QStringLiteral("/old"), "x"));
    const std::optional<QString> second = tmp.create(taskId);
    QCOMPARE(second, first);
    QVERIFY(QDir(*second).isEmpty());
    QVERIFY(!tmp.create(QStringLiteral("../escape")));
    QVERIFY(!tmp.remove(QStringLiteral("..")));
    QVERIFY(!tmp.remove(QStringLiteral("a/b")));
    QVERIFY(tmp.remove(taskId));
    QVERIFY(tmp.remove(taskId)); // nothing left is success
    QVERIFY(tmp.names().isEmpty());
}

} // namespace runner_exec_test
