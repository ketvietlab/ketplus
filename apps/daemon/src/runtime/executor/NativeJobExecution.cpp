#include "runtime/executor/NativeJobSupervisor.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <cerrno>
#include <csignal>
#include <limits>
#include <unistd.h>

namespace ketplus::daemon::executor {
bool supervisorStopRequested();
void NativeJobSupervisor::launch(const QJsonObject& message) {
    const auto execution = message.value("execution").toObject();
    const auto hash = QCryptographicHash::hash(
        QJsonDocument(execution).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    if (launchAttempted_) {
        send(executionHash_ == hash ? "running" : "error",
             executionHash_ == hash ? metadata_ : QJsonObject{{"reason", "idempotency_conflict"}});
        return;
    }
    const auto argv = execution.value("argv").toArray();
    const auto shell = execution.value("shell_command").toString();
    const auto cwd = execution.value("cwd").toString();
    const qint64 remaining = message.value("remaining_timeout_ms").toInteger();
    QString program;
    QStringList arguments;
    if (!argv.isEmpty() && shell.isEmpty()) {
        for (const auto& value : argv) {
            if (!value.isString() || value.toString().contains(QChar::Null)) {
                send("error", {{"reason", "invalid_execution"}});
                return;
            }
            arguments.append(value.toString());
        }
        program = arguments.takeFirst();
    } else if (argv.isEmpty() && !shell.isEmpty()) {
        program = "/bin/sh";
        arguments = {"-c", shell};
    }
    const QFileInfo directory(cwd);
    if (program.isEmpty() || !directory.isDir() || directory.canonicalFilePath() != cwd ||
        remaining <= 0 || remaining > std::numeric_limits<qint64>::max() - clock_.elapsed()) {
        send("error", {{"reason", "invalid_execution"}});
        return;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto delta = execution.value("env_delta").toObject();
    for (auto it = delta.begin(); it != delta.end(); ++it) {
        if (it.key().isEmpty() || it.key().contains('=') || it.key().contains(QChar::Null) ||
            (!it.value().isString() && !it.value().isNull()) ||
            it.value().toString().contains(QChar::Null)) {
            send("error", {{"reason", "invalid_environment_delta"}});
            return;
        }
        if (it.value().isNull())
            environment.remove(it.key());
        else
            environment.insert(it.key(), it.value().toString());
    }
    executionHash_ = hash;
    launchAttempted_ = true;
    deadline_ = clock_.elapsed() + remaining;
    metadata_.insert("launch_attempted", true);
    metadata_.insert("execution_fingerprint", QString::fromLatin1(hash.toHex()));
    metadata_.insert("cwd", cwd);
    // No argv, credential or inherited environment is written to the receipt.
    if (!persist()) {
        rootExited_ = true;
        cleanupError_ = false; // The workload was never started.
        metadata_.insert("error", "handshake_failed");
        finish();
        return;
    }
    process_.setProgram(program);
    process_.setArguments(arguments);
    process_.setWorkingDirectory(cwd);
    process_.setProcessEnvironment(environment);
    process_.start();
}

void NativeJobSupervisor::refreshKnown() {
    bool changed = false;
    const auto previous = known_.values();
    for (const auto& identity : previous) {
        const auto current = inspect(identity.pid);
        if (!current || current->birth != identity.birth || current->zombie) {
            if ((current && current->birth != identity.birth) ||
                (!current && (kill(pid_t(identity.pid), 0) == 0 || errno != ESRCH))) {
                cleanupError_ = true;
                metadata_.insert("cleanup_issue", "process_identity_lost");
            }
            known_.remove(identity.pid);
            changed = true;
        }
    }
    auto parents = known_.values();
    for (qsizetype index = 0; index < parents.size(); ++index) {
        bool complete = false;
        auto pids = ketplus::daemon::executor::children(parents[index].pid, &complete);
        if (!complete && inspect(parents[index].pid)) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "process_snapshot_incomplete");
        }
        if (root_ && parents[index].pid == root_->pid) {
            bool groupComplete = false;
            pids.append(groupMembers(root_->pid, &groupComplete));
            if (!groupComplete && inspect(root_->pid))
                cleanupError_ = true;
        }
        for (const auto pid : pids) {
            if (known_.contains(pid))
                continue;
            const auto child = inspect(pid);
            if (!child || child->zombie ||
                (child->parent != parents[index].pid && (!root_ || child->group != root_->pid)))
                continue;
            if (known_.size() >= 4096) {
                cleanupError_ = true;
                metadata_.insert("cleanup_issue", "process_tracking_capacity_exhausted");
                break;
            }
            known_.insert(pid, *child);
            parents.append(*child);
            changed = true;
        }
    }
    if (changed)
        persist();
}

void NativeJobSupervisor::signalKnown(int signal) {
    refreshKnown();
    // The direct child is owned by QProcess until reaped. Its original group is anchored
    // only while its birth is still valid; detached known descendants are signalled below.
    if (root_ && process_.state() == QProcess::Running) {
        const auto current = inspect(root_->pid);
        if (current && current->birth == root_->birth && current->group == root_->pid &&
            kill(-pid_t(root_->pid), signal) != 0 && errno != ESRCH)
            cleanupError_ = true;
    }
    for (const auto& identity : known_) {
        QString error;
        if (!signalIdentity(identity, signal, &error)) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "process_signal_failed");
        }
    }
}

void NativeJobSupervisor::requestStop(const QString& reason) {
    if (stopping_ || finalized_)
        return;
    stopping_ = true;
    stopReason_ = reason;
    metadata_.insert("cleanup_status", "pending");
    metadata_.insert("stop_reason", reason);
    persist();
    signalKnown(SIGTERM);
    killAt_ = clock_.elapsed() + 1500;
    finishCleanupAt_ = killAt_ + 1500;
    if (!launchAttempted_ || process_.state() == QProcess::NotRunning)
        rootExited_ = true;
    send("stopping", metadata_);
}

void NativeJobSupervisor::tick() {
    refreshKnown();
    if (finalized_) {
        const bool stopped = process_.state() == QProcess::NotRunning;
        if (stopped != metadata_.value("executor_stopped").toBool()) {
            metadata_.insert("executor_stopped", stopped);
            persist();
            send("update", metadata_);
        }
        if (stopped && known_.isEmpty() && clock_.elapsed() - lastPulse_ >= 5000)
            QCoreApplication::quit();
        return;
    }
    if (supervisorStopRequested()) {
        if (launchAttempted_) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "supervisor_termination_requested");
        }
        requestStop("interrupted");
    }
    if (clock_.elapsed() - lastPulse_ >= 5000) {
        if (launchAttempted_) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "lease_expired");
        }
        requestStop("interrupted");
    }
    if (deadline_ > 0 && clock_.elapsed() >= deadline_)
        requestStop("timed_out");
    if (stopping_ && clock_.elapsed() >= killAt_)
        signalKnown(SIGKILL);
    if (stopping_ && clock_.elapsed() >= finishCleanupAt_ &&
        (process_.state() != QProcess::NotRunning || !known_.isEmpty())) {
        cleanupError_ = true;
        metadata_.insert("cleanup_issue", "known_process_survived_cleanup");
        finish();
    } else if (rootExited_ && known_.isEmpty()) {
        finish();
    }
}

void NativeJobSupervisor::finish() {
    if (finalized_)
        return;
    drainOutput();
    finalized_ = true;
    const auto state = cleanupError_            ? "needs_attention"
                       : !stopReason_.isEmpty() ? stopReason_
                       : exitCode_ == 0         ? "completed"
                                                : "failed";
    metadata_.insert("state", state);
    metadata_.insert("exit_code", exitCode_);
    metadata_.insert("executor_stopped", process_.state() == QProcess::NotRunning);
    metadata_.insert("cleanup_status", cleanupError_ ? "unverified"
                                       : metadata_.value("root_started").toBool()
                                           ? "best_effort_complete"
                                           : "not_required");
    if (cleanupError_) {
        metadata_.insert("reason", "cleanup_unverified");
        metadata_.insert("manual_guidance", "Inspect the recorded process identities and "
                                            "descendants, then resolve cleanup in the task UI.");
    }
    if (!persist()) {
        metadata_.insert("state", "needs_attention");
        metadata_.insert("cleanup_status", "unverified");
        metadata_.insert("reason", "cleanup_unverified");
    }
    send("done", metadata_);
}
} // namespace ketplus::daemon::executor
