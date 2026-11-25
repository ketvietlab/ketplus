#include "runtime/ManagedTaskRuntime.h"
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobClient.h"
#include "storage/RemoteJobRows.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QUuid>

namespace ketplus::daemon {
void ManagedTaskRuntime::startProcess(const QString& id) {
    const auto live = live_.value(id);
    if (!live)
        return;
    const auto path = directory_ + '/' + id;
    const auto failBeforeLaunch = [&](const QString& reason) {
        BackgroundTaskChange failed;
        failed.state = "failed";
        failed.reason = reason;
        failed.neverLaunched = true;
        apply(id, failed);
    };
    if (!QDir().mkdir(path) ||
        !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        failBeforeLaunch("spawn_failed");
        return;
    }
    const auto lease = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject identity{{"directory", path}, {"executor_lease_identity", lease}};
#ifdef Q_OS_LINUX
    identity.insert("boot_id", executor::linuxBootId());
    identity.insert("systemd_unit", executor::linuxJobUnit(id, lease));
#endif
    const auto registered =
        ledger_.registerExecutor(id, live->record.value("revision").toInteger(), identity);
    if (!registered.value("ok").toBool())
        return; // No supervisor has launched. Keep the durable intent for reconciliation.
    live->record = registered.value("task").toObject();
    auto* client = new executor::NativeJobClient(executor_, path, id, lease, this);
    live->executor = client;
    client->setRecoveryIdentity(identity);
    const auto remaining = live->record.value("created_at").toInteger() +
                           live->record.value("timeout_ms").toInteger() -
                           QDateTime::currentMSecsSinceEpoch();
    if (remaining <= 0) {
        live->executor = nullptr;
        client->deleteLater();
        stopTask(id, "timed_out");
        return;
    }
    QString error;
    if (!client->start(
            live->execution, remaining,
            [this, id, identity](const QJsonObject& receipt) {
                const auto live = live_.value(id);
                if (!live || live->record.value("state") != "starting")
                    return false;
                BackgroundTaskChange handshake;
                handshake.state = "starting";
                handshake.handshakeCommitted = true;
                handshake.executionIdentity = identity;
                handshake.executionIdentity.insert("supervisor_identity",
                                                   receipt.value("supervisor_identity"));
#ifdef Q_OS_LINUX
                handshake.executionIdentity.insert("linux_boundary",
                                                   receipt.value("linux_boundary"));
#endif
                return apply(id, handshake);
            },
            [this, id](const QJsonObject& event) { observeProcess(id, event); }, &error)) {
        live->executor = nullptr;
        client->deleteLater();
        // start() returns false only before it successfully launches a supervisor.
        failBeforeLaunch("spawn_failed");
    }
    // The executor owns the launch spec now; the journal keeps only approved metadata/hash.
    live->execution = {};
}

void ManagedTaskRuntime::reconcileProcess(const QString& id) {
    const auto live = live_.value(id);
    if (!live)
        return;
    if (remote_job_rows::phaseOf(ledger_.database(), id) == remote_job_rows::Phase::Started)
        return; // the remote jobs service ends or keeps a started remote job
    const auto identity = live->record.value("execution_identity").toObject();
    if (identity.isEmpty()) {
        // The registry commits an executor address before spawning. Its absence
        // proves this intent never authorized a supervisor or workload launch.
        BackgroundTaskChange interrupted;
        interrupted.state = "interrupted";
        interrupted.reason = "daemon_restart_before_launch";
        interrupted.cleanupConfirmed = true;
        interrupted.neverLaunched = true;
        apply(id, interrupted);
        return;
    }
    const auto path = identity.value("directory").toString();
    if (path != directory_ + '/' + id ||
        identity.value("executor_lease_identity").toString().isEmpty()) {
#ifdef Q_OS_LINUX
        BackgroundTaskChange attention;
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "invalid_linux_executor_identity";
        attention.cleanupPending = true;
        attention.executorStopped = false;
        apply(id, attention);
#else
        endUnverified(id, "invalid_executor_identity");
#endif
        return;
    }
    auto* client = new executor::NativeJobClient(
        executor_, path, id, identity.value("executor_lease_identity").toString(), this);
    live->executor = client;
    client->setRecoveryIdentity(identity);
    QString error;
    if (!client->reconnect([this, id](const QJsonObject& event) { observeProcess(id, event); },
                           &error)) {
        live->executor = nullptr;
        client->deleteLater();
#ifdef Q_OS_LINUX
        BackgroundTaskChange attention;
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "linux_executor_reconnect_failed";
        attention.cleanupPending = true;
        attention.executorStopped = false;
        apply(id, attention);
#else
        endUnverified(id, "executor_reconnect_failed");
#endif
        return;
    }
    if (live->record.value("state") == "stopping")
        client->stop(live->record.value("terminal_target").toString());
}

void ManagedTaskRuntime::observeProcess(const QString& id, const QJsonObject& event) {
    const auto live = live_.value(id);
    if (!live)
        return;
    const auto type = event.value("event").toString();
#ifdef Q_OS_LINUX
    if (event.value("cleanup_pending").toBool()) {
        BackgroundTaskChange stopping;
        stopping.state = "stopping";
        stopping.terminalTarget = live->record.value("terminal_target").toString("interrupted");
        stopping.reason = "linux_cleanup_pending";
        stopping.cleanupPending = true;
        apply(id, stopping);
        return;
    }
#endif
    if ((type == "running" || (type == "ready" && event.value("root_started").toBool())) &&
        live->record.value("state") == "starting") {
        auto identity = live->record.value("execution_identity").toObject();
        identity.insert("supervisor_identity", event.value("supervisor_identity"));
        identity.insert("root_identity", event.value("root_identity"));
        identity.insert("process_birth_identity",
                        event.value("root_identity").toObject().value("birth"));
        BackgroundTaskChange running;
        running.state = "running";
        running.executionIdentity = identity;
        running.handshakeCommitted = true;
        apply(id, running);
    }
    if (type != "done" && type != "update")
        return;
    live->pendingExecutorEvent = event;
    auto state = event.value("state").toString();
    auto previous = live->record.value("state").toString();
#ifndef Q_OS_LINUX
    if (event.value("cleanup_issue").toString() == "executor_never_started") {
        // The client sends start only after the handshake is durably committed, so a job
        // without that commit never launched a workload, even if it already waits for a user.
        BackgroundTaskChange change;
        if (!live->record.value("handshake_committed").toBool()) {
            // A stop, owner close or deadline that arrived first keeps the result it asked for,
            // as stopTask does for a job without an executor.
            const QString target =
                previous == "stopping"
                    ? live->record.value("terminal_target").toString("interrupted")
                    : QStringLiteral("failed");
            change.state = target;
            change.reason = target == "failed" ? QStringLiteral("handshake_failed") : target;
            change.errorCode = "executor_never_started";
            change.neverLaunched = true;
            change.cleanupConfirmed = true;
            change.cleanupStatus = "not_required";
            change.summary = "Background task " + id + " ended with state " + target + '.';
        } else {
            // A committed handshake whose receipt vanished is not evidence of anything: the
            // daemon ends the job and records what it could not confirm.
            endUnverified(id, "executor_receipt_unavailable");
            return;
        }
        if (apply(id, change))
            live->pendingExecutorEvent = {};
        pump();
        return;
    }
#endif
    if (state == "needs_attention") {
#ifdef Q_OS_LINUX
        BackgroundTaskChange attention;
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "executor_result_commit_failed";
        attention.cleanupPending = true;
        attention.executorStopped = event.value("executor_stopped").toBool();
        attention.exitCode = event.value("exit_code").toInt(-1);
        attention.errorCode = event.value("cleanup_issue").toString();
        attention.outputSize = event.value("output_size").toInteger();
        attention.outputTruncated = event.value("output_truncated").toBool();
        if (apply(id, attention)) {
            live->pendingExecutorEvent = {};
            if (event.value("executor_stopped").toBool() &&
                event.value("known_processes").toArray().isEmpty() && live->executor) {
                live->executor->acknowledge();
                live->executor->deleteLater();
                live->executor = nullptr;
            }
        }
        return;
#else
        // The executor client keeps probing the job's process group and signalling the
        // identities it verified, so a later result may still verify cleanup. The daemon waits
        // a bounded window for that, then ends the job anyway.
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto issue = event.value("cleanup_issue").toString("cleanup_unverified");
        if (live->settlingSince == 0) {
            live->settlingSince = now;
            qWarning("Job %s: processes not yet confirmed gone (%s); retrying for %lld ms.",
                     qUtf8Printable(id), qUtf8Printable(issue), qint64(cleanupWindowMs_));
            if (previous != "stopping") {
                BackgroundTaskChange settling;
                settling.state = "stopping";
                settling.terminalTarget = realResultState(event);
                settling.reason = "executor_lost";
                settling.cleanupPending = true;
                settling.exitCode = event.value("exit_code").toInt(-1);
                (void)apply(id, settling);
            }
        }
        if (now - live->settlingSince < cleanupWindowMs_)
            return; // pump() presents the pending event again
        endUnverified(id, issue, event);
        return;
#endif
    }
    if (previous == "starting" && event.value("root_started").toBool()) {
        // A transient storage failure may have refused the earlier running event.
        // Reconstruct it from this same verified receipt, without relaunching work.
        auto runningEvent = event;
        runningEvent.insert("event", "running");
        observeProcess(id, runningEvent);
        if (live->record.value("state") == "starting")
            return;
        previous = live->record.value("state").toString();
    }
    if (state == "cancelled" || state == "timed_out") {
        if (previous != "stopping")
            stopTask(id, state);
    }
    if (live->record.value("state") == "stopping")
        state = live->record.value("terminal_target").toString();
    BackgroundTaskChange complete;
    complete.state = state;
    complete.reason = event.value("error").toString();
    complete.cleanupConfirmed = true;
    complete.neverLaunched = !event.value("root_started").toBool();
    applyExecutorCleanupEvidence(complete, event);
#ifdef Q_OS_LINUX
    if (!event.value("linux_boundary").toObject().isEmpty()) {
        complete.executionIdentity = live->record.value("execution_identity").toObject();
        complete.executionIdentity.insert("linux_boundary", event.value("linux_boundary"));
    }
#endif
    complete.exitCode = event.value("exit_code").toInt(-1);
    complete.outputSize = event.value("output_size").toInteger();
    complete.outputTruncated = event.value("output_truncated").toBool();
    complete.summary = "Background task " + id + " ended with state " + state + '.';
    if (apply(id, complete)) {
        live->pendingExecutorEvent = {};
    } else if (live->persistenceError != "storage_error") {
        // A lost running/terminal commit cannot be reported as clean success.
        // Keep execution ownership and the root result available for intervention.
#ifdef Q_OS_LINUX
        BackgroundTaskChange attention;
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "executor_result_commit_failed";
        attention.cleanupPending = true;
        attention.executorStopped = event.value("executor_stopped").toBool();
        attention.exitCode = event.value("exit_code").toInt(-1);
        attention.errorCode = "executor_result_commit_failed";
        if (apply(id, attention))
            live->pendingExecutorEvent = {};
#else
        endUnverified(id, "executor_result_commit_failed", event);
        return;
#endif
    }
    pump();
}

// The state the executor's receipt reports for the command itself when its cleanup is unverified:
// completed or failed when the command ran to an exit, otherwise interrupted.
QString ManagedTaskRuntime::realResultState(const QJsonObject& event) {
    const auto reported = event.value("result_state").toString();
    return reported == "completed" || reported == "failed" ? reported
                                                           : QStringLiteral("interrupted");
}

void ManagedTaskRuntime::endUnverified(const QString& id, const QString& issue,
                                       const QJsonObject& event) {
    const auto live = live_.value(id);
    if (!live)
        return;
    const auto previous = live->record.value("state").toString();
    // The state follows what the command really did: a result the executor reported (completed
    // or failed) is kept, and the cleanup gap goes to the diagnostics only. Without a real
    // result the job is interrupted (ADR 0127 4a).
    const QString target = previous == "stopping"
                               ? live->record.value("terminal_target").toString("interrupted")
                               : realResultState(event);
    const bool keptResult = target == "completed" || target == "failed";
    const bool launched = event.isEmpty() ? live->record.value("handshake_committed").toBool()
                                          : event.value("root_started").toBool(true);
    BackgroundTaskChange change;
    change.state = target;
    change.reason = target == "interrupted" ? QStringLiteral("executor_lost")
                    : keptResult            ? event.value("error").toString()
                                            : target;
    change.cleanupConfirmed = true;
    change.neverLaunched = !launched;
    change.cleanupStatus = launched ? "best_effort_complete" : "not_required";
    change.cleanupDiagnostic = issue;
    change.executorStopped = true;
    if (event.contains("exit_code"))
        change.exitCode = event.value("exit_code").toInt(-1);
    change.outputSize = event.value("output_size").toInteger();
    change.outputTruncated = event.value("output_truncated").toBool();
    change.summary = "Background task " + id + " ended with state " + target + '.';
    qWarning("Job %s ended as %s without confirming its processes are gone: %s.",
             qUtf8Printable(id), qUtf8Printable(target), qUtf8Printable(issue));
    if (!apply(id, change))
        return;
    live->pendingExecutorEvent = {};
    if (live->executor) {
        live->executor->acknowledge();
        live->executor->deleteLater();
        live->executor = nullptr;
    }
    pump();
}
} // namespace ketplus::daemon
