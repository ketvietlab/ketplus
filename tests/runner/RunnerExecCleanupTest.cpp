#include "RunnerExecTest.h"

#include "exec/ExecutorDirectories.h"

#include <QDir>
#include <QFileInfo>
#include <QTest>

namespace runner_exec_test {
namespace {

struct Done {
    int calls = 0;
    bool clean = false;
    QString code;
    std::function<void(bool, const QString&)> callback() {
        return [this](const bool isClean, const QString& leftCode) {
            ++calls;
            clean = isClean;
            code = leftCode;
        };
    }
};

ExecutorDirectories::Entry leave(const Fixture& f, const QString& jobId) {
    const auto entry = ExecutorDirectories(f.executorRoot()).create(jobId, 1);
    Q_ASSERT(entry);
    return *entry;
}

} // namespace

void RunnerExecCleanupTest::nothingLeftIsCleanAtOnce() {
    Fixture f;
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    QTRY_COMPARE(done.calls, 1);
    QVERIFY(done.clean);
    QVERIFY(done.code.isEmpty());
    QVERIFY(f.executor.jobs.isEmpty());
    const QFileInfo root(f.executorRoot());
    QVERIFY(root.isDir());
    QCOMPARE(root.permissions() &
                 (QFile::ReadOther | QFile::WriteOther | QFile::ReadGroup | QFile::WriteGroup),
             QFile::Permissions());
    f.ready();
    QVERIFY(f.runner->admit());
}

void RunnerExecCleanupTest::leftJobIsStoppedAndRemoved() {
    Fixture f;
    const auto entry = leave(f, QStringLiteral("team/job.1/../x"));
    f.ready();
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    QCOMPARE(f.executor.jobs.size(), 1);
    const auto job = f.job();
    QVERIFY(job->reconnected);
    QCOMPARE(job->stops, QStringList{"interrupted"});
    QCOMPARE(job->directory, entry.path);
    QCOMPARE(job->lease, entry.lease);
    QCOMPARE(done.calls, 0);
    QCOMPARE(f.runner->readiness().state, Readiness::State::Ready);
    job->done(result("interrupted", -1, "verified"));
    QTRY_COMPARE(done.calls, 1);
    QVERIFY(done.clean);
    QCOMPARE(job->acknowledged, 1);
    QVERIFY(!QFileInfo::exists(entry.path));
}

void RunnerExecCleanupTest::unverifiedLeftJobIsRetried() {
    Fixture f;
    const auto entry = leave(f, QStringLiteral("job-a"));
    f.ready();
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    f.job()->done(result("interrupted", -1, "best_effort_complete"));
    QTRY_COMPARE(done.calls, 1);
    QVERIFY(!done.clean);
    QCOMPARE(done.code, QStringLiteral("cleanup_unverified"));
    QCOMPARE(f.runner->readiness(),
             (Readiness{Readiness::State::Unavailable, "cleanup_unverified"}));
    QVERIFY(QFileInfo::exists(entry.path));
    QCOMPARE(f.job()->acknowledged, 0);

    QTRY_COMPARE(f.executor.jobs.size(), 2); // retried
    QCOMPARE(f.job()->stops, QStringList{"interrupted"});
    f.job()->done(result("interrupted", -1, "verified"));
    QTRY_VERIFY(f.runner->admit());
    QVERIFY(!QFileInfo::exists(entry.path));
    QCOMPARE(done.calls, 1);
}

void RunnerExecCleanupTest::leftJobWithoutAnAnswerIsKept() {
    Fixture f([](ExecutorCommandRunner::Options& options) {
        options.cleanupWaitMs = 30;
        options.cleanupRetryMs = 60'000;
    });
    const auto entry = leave(f, QStringLiteral("job-a"));
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    QTRY_COMPARE(done.calls, 1);
    QCOMPARE(done.code, QStringLiteral("cleanup_unverified"));
    QVERIFY(QFileInfo::exists(entry.path));
    f.ready();
    QVERIFY(!f.runner->admit());
}

void RunnerExecCleanupTest::unreachableLeftJobIsKept() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.cleanupRetryMs = 60'000; });
    const auto entry = leave(f, QStringLiteral("job-a"));
    f.executor.reconnectOk = false;
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    QTRY_COMPARE(done.calls, 1);
    QCOMPARE(done.code, QStringLiteral("cleanup_unverified"));
    QVERIFY(f.job()->stops.isEmpty());
    QVERIFY(QFileInfo::exists(entry.path));
}

void RunnerExecCleanupTest::unidentifiedDirectoryIsKept() {
    Fixture f([](ExecutorCommandRunner::Options& options) { options.cleanupRetryMs = 60'000; });
    const auto entry = leave(f, QStringLiteral("job-a"));
    QVERIFY(QDir(f.executorRoot()).mkdir(QStringLiteral("stray")));
    f.ready();
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    f.job()->done(result("interrupted", -1, "verified"));
    QTRY_COMPARE(done.calls, 1);
    QCOMPARE(done.code, QStringLiteral("executor_leftover_unidentified"));
    QVERIFY(!QFileInfo::exists(entry.path)); // the identified one is gone
    QVERIFY(QFileInfo::exists(f.executorRoot() + QStringLiteral("/stray")));
    QCOMPARE(f.runner->readiness().code, QStringLiteral("cleanup_unverified"));
}

void RunnerExecCleanupTest::runningCommandIsNotALeftover() {
    Fixture f;
    f.ready();
    f.runner->launch(f.request());
    Done done;
    f.runner->cleanUpLeftovers(done.callback());
    QTRY_COMPARE(done.calls, 1);
    QVERIFY(done.clean);
    QCOMPARE(f.executor.jobs.size(), 1);
    QVERIFY(f.job()->stops.isEmpty());
}

} // namespace runner_exec_test
