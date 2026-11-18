#include "RunnerRunTest.h"

#include "exec/ExecPorts.h"
#include "exec/ExecutorDirectories.h"
#include "run/ExecutorCommandHost.h"

#include <QFileInfo>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>

// The run loop over the executor's real command runner: its readiness alone says when what a
// previous run left is cleaned up.
namespace runner_run_test {
namespace {

// Every left job the command runner reconnects to answers with the next cleanup status.
struct Answers {
    QStringList cleanups; // "best_effort_complete" or "verified", one per reconnect
    int reconnects = 0;
    int acknowledged = 0;
};

class LeftJob final : public exec::ExecutorJob {
  public:
    explicit LeftJob(Answers& answers) : m_answers(answers) {}
    bool start(const QJsonObject&, qint64, Event, QString*) override { return false; }
    bool reconnect(Event event, QString*) override {
        const QString cleanup = m_answers.cleanups.value(m_answers.reconnects++,
                                                         QStringLiteral("best_effort_complete"));
        QTimer::singleShot(0, [event = std::move(event), cleanup] {
            event(QJsonObject{{QStringLiteral("event"), QStringLiteral("done")},
                              {QStringLiteral("state"), QStringLiteral("interrupted")},
                              {QStringLiteral("exit_code"), -1},
                              {QStringLiteral("root_started"), true},
                              {QStringLiteral("cleanup_status"), cleanup}});
        });
        return true;
    }
    void stop(const QString&) override {}
    void acknowledge() override { ++m_answers.acknowledged; }

  private:
    Answers& m_answers;
};

class LeftExecutor final : public exec::Executor {
  public:
    explicit LeftExecutor(Answers& answers) : m_answers(answers) {}
    std::unique_ptr<exec::ExecutorJob> job(const QString&, const QString&,
                                           const QString&) override {
        return std::make_unique<LeftJob>(m_answers);
    }
    std::unique_ptr<exec::OutputSpool> output(const QString&, const QString&) override {
        return nullptr;
    }

  private:
    Answers& m_answers;
};

class OpenPolicy final : public exec::PlatformPolicy {
  public:
    void probe(std::function<void(bool, const QString&)> done) override { done(true, {}); }
};

} // namespace

// The first scan of the left job cannot confirm its clean-up; the command runner's own retry
// does, and the loop then acquires without a restart.
void RunnerRunCleanupTest::retriedCleanUpLetsTheRunnerAcquire() {
    Run run;
    const QString directory = run.state->path() + QStringLiteral("/executor");
    const auto left = exec::ExecutorDirectories(directory).create(QStringLiteral("job_old"), 1);
    QVERIFY(left);
    Answers answers;
    answers.cleanups = {QStringLiteral("best_effort_complete"), QStringLiteral("verified")};
    LeftExecutor executor(answers);
    OpenPolicy policy;
    exec::ExecutorCommandRunner::Options options;
    options.directory = directory;
    options.cleanupRetryMs = 50;
    app::ExecutorCommandHost host(options, executor, policy);
    run.realCommand = &host;
    const auto loopFirst = qScopeGuard([&run] { run.loop.reset(); }); // it holds the host

    run.start();
    QTRY_COMPARE(run.loop->startup(), RunnerLoop::Startup::Running);
    QCOMPARE(answers.reconnects, 1);
    QCOMPARE(host.readiness(), CommandHost::Readiness::Unavailable);
    run.socketReady();
    run.acquire();
    QVERIFY(run.relay.leases.isEmpty());
    QCOMPARE(run.heartbeatState(), QStringLiteral("draining"));

    QTRY_COMPARE(host.readiness(), CommandHost::Readiness::Ready); // retried
    QCOMPARE(answers.reconnects, 2);
    QCOMPARE(answers.acknowledged, 1);
    QVERIFY(!QFileInfo::exists(left->path));
    run.acquire();
    QCOMPARE(run.leaseKinds().value(0), QStringLiteral("acquire"));
    QVERIFY(!run.exit);
}

} // namespace runner_run_test
