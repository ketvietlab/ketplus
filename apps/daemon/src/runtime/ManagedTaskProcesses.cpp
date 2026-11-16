#include "runtime/ManagedTaskRuntime.h"
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobClient.h"

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
    const auto identity = live->record.value("execution_identity").toObject();
    if (identity.isEmpty()) {
        if (live->record.value("state") == "needs_attention") {
            publish(id);
            return;
        }
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
        BackgroundTaskChange attention;
#ifdef Q_OS_LINUX
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "invalid_linux_executor_identity";
        attention.cleanupPending = true;
#else
        attention.state = "needs_attention";
#endif
        attention.executorStopped = false;
        apply(id, attention);
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
        BackgroundTaskChange attention;
#ifdef Q_OS_LINUX
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "linux_executor_reconnect_failed";
        attention.cleanupPending = true;
#else
        attention.state = "needs_attention";
#endif
        attention.executorStopped = false;
        apply(id, attention);
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
    if (state == "needs_attention" || previous == "needs_attention") {
        BackgroundTaskChange attention;
#ifdef Q_OS_LINUX
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "executor_result_commit_failed";
        attention.cleanupPending = true;
#else
        attention.state = "needs_attention";
#endif
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
    complete.cleanupStatus = event.value("cleanup_status").toString();
    complete.fullScopeVerified =
        event.value("containment_mode") == "cgroup_v2" && complete.cleanupStatus == "verified";
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
        BackgroundTaskChange attention;
#ifdef Q_OS_LINUX
        attention.state = "stopping";
        attention.terminalTarget = "interrupted";
        attention.reason = "executor_result_commit_failed";
        attention.cleanupPending = true;
#else
        attention.state = "needs_attention";
#endif
        attention.executorStopped = event.value("executor_stopped").toBool();
        attention.exitCode = event.value("exit_code").toInt(-1);
        attention.errorCode = "executor_result_commit_failed";
        if (apply(id, attention))
            live->pendingExecutorEvent = {};
    }
    pump();
}
} // namespace ketplus::daemon
