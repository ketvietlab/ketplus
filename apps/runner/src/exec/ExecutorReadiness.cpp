#include "exec/ExecLog.h"
#include "exec/ExecutionPlan.h"
#include "exec/ExecutorCommandRunner.h"

#include <QPointer>

#include <algorithm>
#include <utility>

namespace ketplus::runner::exec {
namespace {

const QString containmentUnavailable = QStringLiteral("containment_unavailable");
const QString cleanupUnverified = QStringLiteral("cleanup_unverified");
const QString leftoverUnidentified = QStringLiteral("executor_leftover_unidentified");

} // namespace

void ExecutorCommandRunner::probe() {
    if (m_probing)
        return;
    m_probing = true;
    m_probeTimer.stop();
    m_policy.probe(
        [self = QPointer<ExecutorCommandRunner>(this)](bool available, const QString& code) {
            if (self)
                self->probed(available, code);
        });
}

void ExecutorCommandRunner::probed(const bool available, const QString& code) {
    m_probing = false;
    m_platformAvailable = available;
    m_platformCode = available ? QString() : code.isEmpty() ? containmentUnavailable : code;
    if (!available)
        m_probeTimer.start(m_options.unavailableProbeMs);
    updateReadiness();
}

void ExecutorCommandRunner::updateReadiness() {
    Readiness next;
    if (!m_platformAvailable)
        next = {};
    else if (!*m_platformAvailable)
        next = {Readiness::State::Unavailable, m_platformCode};
    else if (m_unsettled || !m_leftoverCode.isEmpty())
        next = {Readiness::State::Unavailable, cleanupUnverified};
    else
        next = {Readiness::State::Ready, {}};
    if (next == m_readiness)
        return;
    m_readiness = next;
    qCInfo(lcRunnerExec) << "commands" << (next.ready() ? "can run" : "cannot run") << next.code;
    if (m_events.readinessChanged)
        m_events.readinessChanged(next);
}

void ExecutorCommandRunner::cleanUpLeftovers(std::function<void(bool, const QString&)> done) {
    m_cleanupDone = std::move(done);
    scanLeftovers();
}

// Every directory not of the current or unsettled command: its supervisor, if still there, is
// told to stop, and what it or the directory says decides whether the job is gone.
void ExecutorCommandRunner::scanLeftovers() {
    if (m_scanning)
        return;
    m_scanning = true;
    m_cleanupRetry.stop();
    m_unidentified.clear();
    m_leftovers.clear();
    for (const QString& name : m_directories.names()) {
        if ((m_running && m_running->entry.taskId == name) ||
            (m_unsettled && m_unsettled->entry.taskId == name))
            continue;
        const std::optional<ExecutorDirectories::Entry> entry = m_directories.read(name);
        if (!entry) {
            m_unidentified.append(name);
            continue;
        }
        m_leftovers.push_back(
            {*entry, m_executor.job(entry->path, entry->taskId, entry->lease), {}});
    }
    for (Leftover& leftover : m_leftovers) {
        const QString taskId = leftover.entry.taskId;
        QString error;
        if (!leftover.job->reconnect(
                [this, taskId](const QJsonObject& event) { leftoverEvent(taskId, event); },
                &error)) {
            leftover.clean = false;
            continue;
        }
        leftover.job->stop(QStringLiteral("interrupted"));
    }
    const bool decided =
        std::all_of(m_leftovers.cbegin(), m_leftovers.cend(),
                    [](const Leftover& leftover) { return leftover.clean.has_value(); });
    if (decided)
        finishScanSoon();
    else
        m_cleanupWait.start(m_options.cleanupWaitMs);
}

void ExecutorCommandRunner::leftoverEvent(const QString& taskId, const QJsonObject& event) {
    if (!m_scanning || event.value(QStringLiteral("event")).toString() != QStringLiteral("done"))
        return;
    bool decided = true;
    for (Leftover& leftover : m_leftovers) {
        if (leftover.entry.taskId == taskId && !leftover.clean)
            leftover.clean = commandExit(event).cleanupStatus == QStringLiteral("verified");
        decided = decided && leftover.clean.has_value();
    }
    if (decided)
        finishScanSoon();
}

void ExecutorCommandRunner::leftoversExpired() {
    if (m_scanning)
        finishScan();
}

// After the current turn of the event loop: a client may be inside its own event.
void ExecutorCommandRunner::finishScanSoon() {
    QTimer::singleShot(0, this, [this] {
        if (m_scanning)
            finishScan();
    });
}

void ExecutorCommandRunner::finishScan() {
    m_cleanupWait.stop();
    bool unclean = false;
    for (Leftover& leftover : m_leftovers) {
        if (leftover.clean.value_or(false)) {
            leftover.job->acknowledge();
            m_directories.remove(leftover.entry);
            removeTmp(leftover.entry.taskId);
            qCInfo(lcRunnerExec) << "job" << leftover.entry.jobId << "attempt"
                                 << leftover.entry.attempt << "left by a restart: cleaned up";
        } else {
            unclean = true; // the directory stays as it is
        }
    }
    m_leftovers.clear();
    m_scanning = false;
    sweepTmp();
    m_leftoverCode = !m_unidentified.isEmpty() ? leftoverUnidentified
                     : unclean                 ? cleanupUnverified
                                               : QString();
    if (m_leftoverCode.isEmpty()) {
        m_reportedLeftover.clear();
    } else {
        if (m_leftoverCode != m_reportedLeftover)
            qCWarning(lcRunnerExec) << "job directories left by a restart:" << m_leftoverCode;
        m_reportedLeftover = m_leftoverCode;
        m_cleanupRetry.start(m_options.cleanupRetryMs);
    }
    updateReadiness();
    if (m_cleanupDone)
        std::exchange(m_cleanupDone, {})(m_leftoverCode.isEmpty(), m_leftoverCode);
}

// A job tmp directory whose job directory is gone belongs to no job any more. One of a job still
// left stays until that job's cleanup is confirmed.
void ExecutorCommandRunner::sweepTmp() {
    if (!m_tmp)
        return;
    const QStringList jobs = m_directories.names();
    for (const QString& name : m_tmp->names()) {
        if ((m_running && m_running->entry.taskId == name) ||
            (m_unsettled && m_unsettled->entry.taskId == name) || jobs.contains(name))
            continue;
        removeTmp(name);
    }
}

} // namespace ketplus::runner::exec
