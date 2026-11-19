#include "exec/ExecutorCommandRunner.h"
#include "exec/ExecLog.h"
#include "exec/ExecutionPlan.h"

#include <QPointer>
#include <QProcessEnvironment>

namespace ketplus::runner::exec {
namespace {

constexpr int pageBytes = 8 * 1024;
constexpr int maxReadFailures = 20;
const QString tmpRoot = QStringLiteral("/jobtmp");

QMap<QString, QString> systemEnvironment() {
    QMap<QString, QString> environment;
    const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
    for (const QString& name : system.keys())
        environment.insert(name, system.value(name));
    return environment;
}

} // namespace

ExecutorCommandRunner::ExecutorCommandRunner(Options options, Executor& executor,
                                             PlatformPolicy& policy, Events events, QObject* parent)
    : QObject(parent), m_options(std::move(options)), m_executor(executor), m_policy(policy),
      m_events(std::move(events)), m_directories(m_options.directory) {
    if (!m_options.environment)
        m_options.environment = systemEnvironment;
    if (m_options.mounts)
        m_tmp.emplace(m_options.mounts->work + tmpRoot);
    for (QTimer* timer : {&m_poll, &m_killWait, &m_probeTimer, &m_cleanupWait, &m_cleanupRetry})
        timer->setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &ExecutorCommandRunner::poll);
    connect(&m_killWait, &QTimer::timeout, this, &ExecutorCommandRunner::killExpired);
    connect(&m_probeTimer, &QTimer::timeout, this, &ExecutorCommandRunner::probe);
    connect(&m_cleanupWait, &QTimer::timeout, this, &ExecutorCommandRunner::leftoversExpired);
    connect(&m_cleanupRetry, &QTimer::timeout, this, &ExecutorCommandRunner::scanLeftovers);
}

// A supervisor whose client goes away stops its command when the client's lease expires.
ExecutorCommandRunner::~ExecutorCommandRunner() = default;

bool ExecutorCommandRunner::admit() { return m_readiness.ready(); }

void ExecutorCommandRunner::fail(const session::LaunchRequest& request, const QString& reason) {
    qCInfo(lcRunnerExec) << "job" << request.jobId << "attempt" << request.attempt
                         << "not launched:" << reason;
    session::CommandExit exit;
    exit.state = QStringLiteral("failed");
    exit.reason = reason;
    exit.cleanupStatus = QStringLiteral("verified"); // nothing ran
    if (m_events.finished)
        m_events.finished(exit);
}

// The launch with the job's mounts, once its executor directory holds the task id: a tmp
// directory of the same name can belong to no other job.
PreparedLaunch ExecutorCommandRunner::prepareMounted(const session::LaunchRequest& request,
                                                     const QString& taskId) {
    const std::optional<QString> tmp = m_tmp->create(taskId);
    if (!tmp)
        return {{}, QStringLiteral("tmp_directory_unavailable")};
    const Mounts& roots = *m_options.mounts;
    const MountPlan plan =
        planMounts(roots.state, roots.work, roots.runtime, request.workspace, *tmp);
    PreparedLaunch prepared = prepareLaunch(request, m_options.environment(), &plan);
    if (!prepared.error.isEmpty())
        removeTmp(taskId);
    return prepared;
}

void ExecutorCommandRunner::removeTmp(const QString& taskId) {
    if (m_tmp && !m_tmp->remove(taskId))
        qCWarning(lcRunnerExec) << "a job tmp directory could not be removed";
}

void ExecutorCommandRunner::launch(const session::LaunchRequest& request) {
    if (m_running) {
        // The session runs one command at a time; the running one is left alone.
        qCWarning(lcRunnerExec) << "job" << request.jobId << "launch refused: a command runs";
        return;
    }
    if (!m_readiness.ready()) {
        fail(request, m_readiness.code.isEmpty() ? QStringLiteral("containment_unavailable")
                                                 : m_readiness.code);
        return;
    }
    PreparedLaunch prepared = prepareLaunch(request, m_options.environment());
    if (!prepared.error.isEmpty()) {
        fail(request, prepared.error);
        return;
    }
    const std::optional<ExecutorDirectories::Entry> entry =
        m_directories.create(request.jobId, request.attempt);
    if (!entry) {
        fail(request, QStringLiteral("executor_directory_unavailable"));
        return;
    }
    if (m_options.mounts) {
        prepared = prepareMounted(request, entry->taskId);
        if (!prepared.error.isEmpty()) {
            m_directories.remove(*entry); // nothing was started from it
            fail(request, prepared.error);
            return;
        }
    }
    auto running = std::make_unique<Running>();
    running->entry = *entry;
    running->job = m_executor.job(entry->path, entry->taskId, entry->lease);
    running->output = m_executor.output(entry->path, entry->taskId);
    running->clock.start();
    const Running* key = running.get();
    m_running = std::move(running);
    QString error;
    const bool started = m_running->job->start(
        prepared.execution, request.timeoutMs + m_options.backstopMs,
        [this, key](const QJsonObject& event) { onEvent(key, event); }, &error);
    if (!started) {
        // No supervisor was started: nothing can run from this directory.
        const ExecutorDirectories::Entry gone = m_running->entry;
        m_running.reset();
        m_directories.remove(gone);
        removeTmp(gone.taskId);
        fail(request, QStringLiteral("spawn_failed"));
        return;
    }
    qCInfo(lcRunnerExec) << "job" << request.jobId << "attempt" << request.attempt
                         << "command launched";
    m_poll.start(m_options.pollMs);
}

void ExecutorCommandRunner::kill() {
    if (!m_running || m_running->killed || m_running->result)
        return;
    m_running->killed = true;
    m_running->job->stop(QStringLiteral("cancelled"));
    m_killWait.start(m_options.killWaitMs);
}

// Events may come from inside the executor's client: its objects are never deleted here.
void ExecutorCommandRunner::onEvent(const Running* running, const QJsonObject& event) {
    if (event.value(QStringLiteral("event")).toString() != QStringLiteral("done"))
        return;
    if (m_unsettled && m_unsettled.get() == running) {
        if (commandExit(event).cleanupStatus == QStringLiteral("verified"))
            settleLater();
        return;
    }
    if (!m_running || m_running.get() != running || m_running->result)
        return;
    m_running->result = event;
    m_killWait.stop();
    m_poll.start(0); // every byte left in the store, then the end
}

// True when the last page read was full: more may follow at once.
bool ExecutorCommandRunner::readOutput(const int pages) {
    for (int read = 0; read < pages && m_running; ++read) {
        const OutputSpool::Page page = m_running->output->read(pageBytes);
        if (page.failed) {
            ++m_running->readFailures;
            return false;
        }
        m_running->readFailures = 0;
        if (page.lost > 0) {
            m_running->outputBytes += page.lost;
            if (m_events.outputLost)
                m_events.outputLost(page.lost);
        }
        if (!m_running)
            return false;
        if (!page.bytes.isEmpty()) {
            m_running->outputBytes += page.bytes.size();
            if (m_events.output)
                m_events.output(page.bytes);
        }
        if (page.bytes.size() < pageBytes)
            return false;
    }
    return true;
}

void ExecutorCommandRunner::poll() {
    if (!m_running)
        return;
    const bool more = readOutput(m_options.pagesPerPoll);
    if (!m_running)
        return;
    if (!m_running->result) {
        m_poll.start(more ? 0 : m_options.pollMs);
        return;
    }
    if (more || (m_running->readFailures > 0 && m_running->readFailures < maxReadFailures)) {
        m_poll.start(more ? 0 : m_options.pollMs);
        return;
    }
    report(commandExit(*m_running->result));
}

// The executor did not confirm the end in time: the command is reported once, with its cleanup
// unverified, and its directory stays.
void ExecutorCommandRunner::killExpired() {
    if (!m_running || m_running->result)
        return;
    while (readOutput(m_options.pagesPerPoll) && m_running) {
    }
    if (!m_running)
        return;
    session::CommandExit exit;
    exit.state = QStringLiteral("failed");
    exit.reason = QStringLiteral("cleanup_unverified");
    exit.cleanupStatus = QStringLiteral("unverified");
    report(exit);
}

void ExecutorCommandRunner::report(const session::CommandExit& exit) {
    m_poll.stop();
    m_killWait.stop();
    std::unique_ptr<Running> running = std::move(m_running);
    running->reported = true;
    running->output.reset();
    const bool verified = exit.cleanupStatus == QStringLiteral("verified") && running->result;
    qCInfo(lcRunnerExec) << "job" << running->entry.jobId << "attempt" << running->entry.attempt
                         << "command ended:" << (exit.reason.isEmpty() ? exit.state : exit.reason)
                         << "cleanup" << exit.cleanupStatus << "output bytes"
                         << running->outputBytes << "after" << running->clock.elapsed() << "ms";
    if (verified) {
        running->job->acknowledge();
        m_directories.remove(running->entry);
        removeTmp(running->entry.taskId);
        // The client may be inside its own event: it goes after this turn of the loop.
        QTimer::singleShot(0, this,
                           [job = std::shared_ptr<ExecutorJob>(std::move(running->job))] {});
    } else {
        m_unsettled = std::move(running);
    }
    updateReadiness();
    if (m_events.finished)
        m_events.finished(exit);
    probe();
}

void ExecutorCommandRunner::settleLater() {
    QTimer::singleShot(0, this, [this] {
        if (!m_unsettled)
            return;
        m_unsettled->job->acknowledge();
        m_directories.remove(m_unsettled->entry);
        removeTmp(m_unsettled->entry.taskId);
        qCInfo(lcRunnerExec) << "job" << m_unsettled->entry.jobId << "cleanup confirmed";
        m_unsettled.reset();
        updateReadiness();
    });
}

} // namespace ketplus::runner::exec
