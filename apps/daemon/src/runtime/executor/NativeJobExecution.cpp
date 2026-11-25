#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobLease.h"
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
#ifdef Q_OS_LINUX
    // Only the trusted helper executes before namespace isolation. Supplying
    // workload env here would permit loader injection ahead of that boundary.
    process_.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process_.setProgram(QCoreApplication::applicationFilePath());
    process_.setArguments({"--linux-launch"});
    process_.setStandardInputFile({});
#endif
    QString gateError;
    if (!prepareLaunchGate(&gateError)) {
        metadata_.insert("error", "handshake_failed");
        rootExited_ = true;
        finish();
        return;
    }
    process_.start();
#ifdef Q_OS_LINUX
    auto privateExecution = execution;
    privateExecution.insert("_base_environment", message.value("base_environment"));
    process_.write(QJsonDocument(privateExecution).toJson(QJsonDocument::Compact));
    process_.closeWriteChannel();
#endif
    // Only the forked child keeps these ends. EOF on the gate means abort,
    // including an abrupt supervisor crash before committing root identity.
    close(launchIdentityPipe_[1]);
    launchIdentityPipe_[1] = -1;
    close(launchGate_[0]);
    launchGate_[0] = -1;
}

void NativeJobSupervisor::refreshKnown() {
#ifdef Q_OS_LINUX
    // Discovery is display evidence only. The cgroup, rather than a sampled
    // process list, defines membership and cleanup completeness on Linux.
    if (root_) {
        const auto current = inspect(root_->pid);
        if (!current || current->birth != root_->birth || current->zombie)
            known_.remove(root_->pid);
    }
    return;
#endif
    bool changed = false;
    const auto previous = known_.values();
    for (const auto& identity : previous) {
        const auto current = inspect(identity.pid);
        if (!current || current->birth != identity.birth || current->zombie) {
            if ((current && current->birth != identity.birth) ||
                (!current && (kill(pid_t(identity.pid), 0) == 0 || errno != ESRCH))) {
                cleanupError_ = true;
                metadata_.insert("cleanup_issue", "process_identity_lost");
                if (!current)
                    continue; // Uninspectable is not evidence the known workload stopped.
            }
            known_.remove(identity.pid);
            changed = true;
        }
    }
    auto parents = known_.values();
    for (qsizetype index = 0; index < parents.size(); ++index) {
        const auto parent = inspect(parents[index].pid);
        if (!parent || parent->birth != parents[index].birth || parent->zombie)
            continue;
        bool complete = false;
        auto pids = ketplus::daemon::executor::children(parents[index].pid, &complete);
        const auto afterSnapshot = inspect(parents[index].pid);
        if (!complete && afterSnapshot && !afterSnapshot->zombie &&
            afterSnapshot->birth == parents[index].birth) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "process_snapshot_incomplete");
        }
        if (root_ && parents[index].pid == root_->pid) {
            bool groupComplete = false;
            pids.append(groupMembers(root_->pid, &groupComplete));
            const auto afterGroup = inspect(root_->pid);
            if (!groupComplete && afterGroup && !afterGroup->zombie &&
                afterGroup->birth == root_->birth) {
                cleanupError_ = true;
                metadata_.insert("cleanup_issue", "process_group_snapshot_incomplete");
            }
        }
        for (const auto pid : pids) {
            if (known_.contains(pid))
                continue;
            const auto child = inspect(pid);
            const auto parentNow = inspect(parents[index].pid);
            const auto rootNow = root_ ? inspect(root_->pid) : std::nullopt;
            const bool direct = child && parentNow && parentNow->birth == parents[index].birth &&
                                child->parent == parentNow->pid;
            const bool grouped = child && rootNow && rootNow->birth == root_->birth &&
                                 child->group == rootNow->group && rootNow->group == rootNow->pid;
            if (!child || child->zombie || (!direct && !grouped))
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
#ifdef Q_OS_LINUX
    if (signal == SIGKILL) {
        if (!linuxKillBoundary(metadata_.value("linux_boundary").toObject()))
            metadata_.insert("cleanup_issue", "cgroup_kill_failed");
        return;
    }
#endif
    refreshKnown();
    // Group membership is discovery evidence, not an atomic signal target.
    // Signal each recorded incarnation; Linux escalates through cgroup.kill.
    for (const auto& identity : known_) {
        QString error;
        if (!signalIdentity(identity, signal, &error)) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "process_signal_failed");
            metadata_.insert("cleanup_error", error);
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
#ifdef Q_OS_LINUX
    linuxTick();
    return;
#endif
    refreshKnown();
    if (finalized_) {
        const bool stopped = process_.state() == QProcess::NotRunning && known_.isEmpty();
        if (stopped != metadata_.value("executor_stopped").toBool()) {
            metadata_.insert("executor_stopped", stopped);
            persist();
            send("update", metadata_);
        }
        if (stopped && known_.isEmpty() && clock_.elapsed() - lastPulse_ >= NativeJobLeaseMs)
            QCoreApplication::quit();
        return;
    }
    // Losing the daemon is a reason to stop, not evidence of a leak. Whether
    // anything survived is decided by the OS: the process group (verifyGroupEmpty).
    if (supervisorStopRequested()) {
        if (stopCause_.isEmpty())
            stopCause_ = "supervisor_termination_requested";
        requestStop("interrupted");
    }
    if (leaseOrChannelExpired())
        requestStop("interrupted");
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

bool NativeJobSupervisor::leaseOrChannelExpired() {
    const auto now = clock_.elapsed();
    QString cause;
    if (channelLostAt_ >= 0 && now - channelLostAt_ >= 1000)
        cause = "channel_closed";
    else if (checkLease())
        cause = "lease_expired";
    if (cause.isEmpty())
        return false;
    if (stopCause_.isEmpty())
        stopCause_ = cause;
    return true;
}

bool NativeJobSupervisor::verifyGroupEmpty() {
    if (!root_)
        return !launchAttempted_ || !metadata_.value("root_started").toBool();
    return inspectGroup(root_->group) == GroupState::Empty;
}

void NativeJobSupervisor::finish() {
    if (finalized_)
        return;
    drainOutput();
#ifdef Q_OS_LINUX
    if (!linuxBoundaryEmpty(metadata_.value("linux_boundary").toObject()) ||
        process_.state() != QProcess::NotRunning) {
        metadata_.insert("cleanup_status", "pending");
        metadata_.insert("cleanup_pending", true);
        metadata_.insert("cleanup_issue", "linux_cleanup_pending");
        persist();
        send("update", metadata_);
        return;
    }
    // Cleanup uncertainty never becomes a terminal result. Kernel populated=0
    // is the final fence; earlier lease/signal errors cannot weaken it.
    cleanupError_ = false;
#endif
#ifndef Q_OS_LINUX
    if (!stopCause_.isEmpty()) {
        metadata_.insert("stop_cause", stopCause_);
        if (!cleanupError_ && !verifyGroupEmpty()) {
            cleanupError_ = true;
            metadata_.insert("cleanup_issue", "process_group_not_verified_empty");
        }
    }
#endif
    finalized_ = true;
    const auto state = cleanupError_            ? "needs_attention"
                       : !stopReason_.isEmpty() ? stopReason_
                       : exitCode_ == 0         ? "completed"
                                                : "failed";
    metadata_.insert("state", state);
    metadata_.insert("exit_code", exitCode_);
    metadata_.insert("executor_stopped",
                     process_.state() == QProcess::NotRunning && known_.isEmpty());
    metadata_.insert("cleanup_status", cleanupError_ ? "unverified"
                                       : metadata_.value("root_started").toBool()
                                           ? "best_effort_complete"
                                           : "not_required");
#ifdef Q_OS_LINUX
    metadata_.insert("cleanup_status", "verified");
    metadata_.insert("cleanup_pending", false);
#else
    if (!cleanupError_ && !stopCause_.isEmpty() && metadata_.value("root_started").toBool()) {
        // The daemon went away but the OS reports no live member in the job group.
        metadata_.insert("cleanup_status", "verified");
        metadata_.insert("cleanup_verification", "process_group_empty");
        qInfo("Job cleanup verified by empty process group after %s.", qUtf8Printable(stopCause_));
    }
#endif
    if (cleanupError_) {
        metadata_.insert("reason", "cleanup_unverified");
        metadata_.insert("manual_guidance", "Inspect the recorded process identities and "
                                            "descendants, then resolve cleanup in the task UI.");
    }
    if (!persist()) {
#ifdef Q_OS_LINUX
        finalized_ = false;
        metadata_.remove("state");
        metadata_.insert("cleanup_pending", true);
        return;
#else
        metadata_.insert("state", "needs_attention");
        metadata_.insert("cleanup_status", "unverified");
        metadata_.insert("reason", "cleanup_unverified");
#endif
    }
    send("done", metadata_);
}
} // namespace ketplus::daemon::executor
