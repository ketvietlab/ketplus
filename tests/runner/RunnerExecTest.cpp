#include "RunnerExecTest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTest>

namespace runner_exec_test {
namespace {

class FakeJob final : public ExecutorJob {
  public:
    explicit FakeJob(std::shared_ptr<JobState> state) : m_state(std::move(state)) {}
    ~FakeJob() override { m_state->alive = false; }

    bool start(const QJsonObject& execution, const qint64 timeoutMs, Event event,
               QString* error) override {
        m_state->execution = execution;
        m_state->timeoutMs = timeoutMs;
        m_state->event = std::move(event);
        if (!m_state->startOk && error)
            *error = QStringLiteral("start failed");
        return m_state->startOk;
    }
    bool reconnect(Event event, QString*) override {
        m_state->reconnected = true;
        m_state->event = std::move(event);
        return m_state->reconnectOk;
    }
    void stop(const QString& reason) override { m_state->stops.append(reason); }
    void acknowledge() override { ++m_state->acknowledged; }

  private:
    std::shared_ptr<JobState> m_state;
};

class FakeSpool final : public OutputSpool {
  public:
    explicit FakeSpool(std::shared_ptr<SpoolState> state) : m_state(std::move(state)) {
        m_state->clock.start();
    }
    Page read(const int maxBytes) override {
        m_state->readAt.append(m_state->clock.elapsed());
        if (m_state->pages.isEmpty())
            return {};
        Page next = m_state->pages.takeFirst();
        Q_ASSERT(next.bytes.size() <= maxBytes);
        return next;
    }

  private:
    std::shared_ptr<SpoolState> m_state;
};

} // namespace

std::unique_ptr<ExecutorJob> FakeExecutor::job(const QString& directory, const QString& taskId,
                                               const QString& lease) {
    auto state = std::make_shared<JobState>();
    state->directory = directory;
    state->taskId = taskId;
    state->lease = lease;
    state->startOk = startOk;
    state->reconnectOk = reconnectOk;
    jobs.append(state);
    return std::make_unique<FakeJob>(state);
}

std::unique_ptr<OutputSpool> FakeExecutor::output(const QString&, const QString&) {
    auto state = std::make_shared<SpoolState>();
    state->pages = std::exchange(nextPages, {});
    spools.append(state);
    return std::make_unique<FakeSpool>(state);
}

void ManualPolicy::probe(std::function<void(bool, const QString&)> done) {
    ++probes;
    if (autoAnswer)
        done(*autoAnswer, autoCode);
    else
        m_pending.append(std::move(done));
}

void ManualPolicy::answer(const bool available, const QString& code) {
    for (const auto& done : std::exchange(m_pending, {}))
        done(available, code);
}

Fixture::Fixture(const std::function<void(ExecutorCommandRunner::Options&)>& tweak) {
    workspace = QFileInfo(temp.path()).canonicalFilePath() + QStringLiteral("/ws");
    QDir().mkpath(workspace);
    ExecutorCommandRunner::Options options;
    options.directory = executorRoot();
    options.environment = [] {
        return QMap<QString, QString>{{QStringLiteral("RUNNER_ONLY"), QStringLiteral("1")}};
    };
    options.pollMs = 5;
    options.killWaitMs = 200;
    options.unavailableProbeMs = 50;
    options.cleanupRetryMs = 50;
    options.cleanupWaitMs = 200;
    if (tweak)
        tweak(options);
    ExecutorCommandRunner::Events callbacks;
    callbacks.output = [this](const QByteArray& bytes) {
        events.append(QStringLiteral("output %1").arg(bytes.size()));
        output += bytes;
    };
    callbacks.outputLost = [this](const qint64 bytes) {
        events.append(QStringLiteral("lost %1").arg(bytes));
    };
    callbacks.finished = [this](const session::CommandExit& exit) {
        events.append(
            QStringLiteral("finished %1 %2 %3").arg(exit.state, exit.reason, exit.cleanupStatus));
        exits.append(exit);
    };
    callbacks.readinessChanged = [this](const Readiness& next) { readiness.append(next); };
    runner = std::make_unique<ExecutorCommandRunner>(options, executor, policy, callbacks);
}

session::LaunchRequest Fixture::request(const QString& jobId) const {
    session::LaunchRequest launch;
    launch.jobId = jobId;
    launch.attempt = 1;
    launch.timeoutMs = 1000;
    launch.invocation.argv = QStringList{QStringLiteral("make")};
    launch.workspace = workspace;
    launch.environment = {{QStringLiteral("JOB"), QStringLiteral("1")}};
    return launch;
}

void Fixture::ready() {
    runner->probe();
    policy.answer(true);
}

OutputSpool::Page page(QByteArray bytes, const qint64 lost) {
    OutputSpool::Page next;
    next.bytes = std::move(bytes);
    next.lost = lost;
    return next;
}

QJsonObject result(const char* state, const int exitCode, const char* cleanup, const bool started) {
    return {{"state", state},
            {"root_started", started},
            {"exit_code", exitCode},
            {"cleanup_status", cleanup}};
}

} // namespace runner_exec_test

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        runner_exec_test::RunnerExecRunnerTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        runner_exec_test::RunnerExecCleanupTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    return status;
}
