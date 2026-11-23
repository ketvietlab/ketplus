#include <QtGlobal>
// POSIX-only; Windows builds NativeJobUnsupported.cpp instead.
#if !defined(Q_OS_WIN)
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobClient.h"
#include "runtime/executor/NativeJobControlAddress.h"
#include "runtime/executor/NativeProcessIdentity.h"

#include <QFileInfo>
#include <QJsonArray>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace ketplus::daemon::executor {
namespace {
[[maybe_unused]] NativeProcessIdentity fromJson(const QJsonObject& value) {
    return {value.value("pid").toInteger(), value.value("parent_pid").toInteger(),
            value.value("process_group").toInteger(), value.value("birth").toString()};
}
bool alive(const QJsonObject& value) {
    const auto current = inspect(value.value("pid").toInteger());
    return current && !current->zombie && current->birth == value.value("birth").toString();
}
bool stopped(const QJsonObject& value) {
    if (value.value("pid").toInteger() <= 0 || value.value("birth").toString().isEmpty())
        return false;
    const auto current = inspect(value.value("pid").toInteger());
    if (current)
        return current->zombie || current->birth != value.value("birth").toString();
    return kill(pid_t(value.value("pid").toInteger()), 0) != 0 && errno == ESRCH;
}
} // namespace

void NativeJobClient::recoverLostSupervisor() {
#ifdef Q_OS_LINUX
    if (managerPending_)
        return;
    const auto persisted = receipt();
    if (!persisted.isEmpty())
        lastReceipt_ = persisted;
    const auto saved = lastReceipt_;
    const auto state = saved.value("state").toString();
    const bool terminalReceipt =
        !state.isEmpty() && state != "needs_attention" && state != "stopping";
    // Fence launch before yielding to the manager. A late ready cannot replay it.
    stopReason_ = "interrupted";
    sentStart_ = true;
    execution_ = {};
    inheritedEnvironment_ = {};
    if (recoveryStarted_ < 0)
        recoveryStarted_ = clock_.elapsed();
    managerPending_ = true;
    linuxUnitPropertiesAsync(
        linuxJobUnit(taskId_, lease_), this,
        [this, saved, terminalReceipt](const QJsonObject& service) {
            managerPending_ = false;
            if (acknowledged_)
                return;
            auto identity = recoveryIdentity_;
            if (identity.value("linux_boundary").toObject().isEmpty() &&
                linuxReceiptTrusted(taskId_, lease_, saved, service))
                identity.insert("linux_boundary", saved.value("linux_boundary"));
            const auto cleanup = linuxRecoverBoundary(taskId_, lease_, identity, service);
            const bool clean = cleanup.value("clean").toBool();
            const auto reason = cleanup.value("reason").toString();
            if (!clean && recoveryReported_ && recoveryIssue_ == reason)
                return;
            recoveryReported_ = true;
            recoveryIssue_ = reason;
            auto result = saved;
            result.insert("task_id", taskId_);
            result.insert("executor_lease_identity", lease_);
            result.insert("containment_mode", "cgroup_v2");
            result.insert("event", clean ? "done" : "update");
            if (!terminalReceipt || !clean)
                result.insert("state", clean ? "interrupted" : "stopping");
            result.insert("cleanup_status", clean ? "verified" : "pending");
            result.insert("cleanup_pending", !clean);
            result.insert("executor_stopped", clean);
            result.insert("error", reason);
            if (event_)
                event_(result);
            finished_ = clean;
        });
#else
    const auto persisted = receipt();
    if (persisted.isEmpty() && lastReceipt_.isEmpty() && !authenticated_ &&
        !QFileInfo::exists(directory_ + "/executor.json") &&
        !QFileInfo::exists(controlAddress(directory_, lease_))) {
        // The supervisor persists its receipt before it can launch anything and only then
        // listens. A workload starts only on a command over that socket, sent after the daemon
        // committed the handshake. With neither file present, and no receipt ever seen by this
        // client, the supervisor never got that far, so no workload exists. A client that saw a
        // receipt keeps signalling the identities it holds. The runtime still checks the
        // handshake before ending the job.
        reportNeverStarted();
        return;
    }
    if (!persisted.isEmpty())
        lastReceipt_ = persisted;
    const auto state = lastReceipt_.value("state").toString();
    if (!state.isEmpty() && state != "needs_attention" && state != "stopping" &&
        recoveryStarted_ < 0) {

        auto result = lastReceipt_;
        result.insert("event", "done");
        if (!finished_)
            message(result); // The executor's durable result survives a lost reply.
        return;
    }
    if (recoveryStarted_ < 0)
        recoveryStarted_ = clock_.elapsed();

    const int signal = clock_.elapsed() - recoveryStarted_ >= 1500 ? SIGKILL : SIGTERM;
    // Inspect every identity again immediately before signalling. Never adopt or
    // signal a process just because its PID or process group matches an old row.
    const auto identities = lastReceipt_.value("known_processes").toArray();
    for (const auto value : identities) {
        QString error;
        signalIdentity(fromJson(value.toObject()), signal, &error);
    }
    const auto root = lastReceipt_.value("root_identity").toObject();
    if (!root.isEmpty()) {
        QString error;
        signalIdentity(fromJson(root), signal, &error);
    }
    const auto supervisor = lastReceipt_.value("supervisor_identity").toObject();
    if (!supervisor.isEmpty() && alive(supervisor)) {
        QString error;
        signalIdentity(fromJson(supervisor), signal, &error);
    }
    // Missing receipts cannot establish that an as-yet-unregistered supervisor
    // stopped. Keep the slot and attention record rather than inventing evidence.
    bool knownStopped =
        !supervisor.isEmpty() && stopped(supervisor) && (root.isEmpty() || stopped(root));
    for (const auto value : identities)
        knownStopped = knownStopped && stopped(value.toObject());
    reportLost(knownStopped, lastReceipt_.isEmpty() ? "executor_receipt_unavailable"
                                                    : "executor_connection_lost");
#endif
}

void NativeJobClient::reportNeverStarted() {
    if (recoveryReported_ && recoveryIssue_ == "executor_never_started")
        return;
    recoveryReported_ = true;
    recoveryStopped_ = true;
    recoveryIssue_ = "executor_never_started";
    lastReceipt_ = {{"task_id", taskId_},
                    {"executor_lease_identity", lease_},
                    {"event", "done"},
                    {"state", "failed"},
                    {"error", "handshake_failed"},
                    {"cleanup_issue", recoveryIssue_},
                    {"cleanup_status", "not_required"},
                    {"containment_mode", "native_best_effort"},
                    {"root_started", false},
                    {"executor_stopped", true},
                    {"known_processes", QJsonArray{}}};
    finished_ = true;
    if (event_)
        event_(lastReceipt_);
}

void NativeJobClient::reportLost(bool stopped, const QString& issue) {
    const bool changed =
        !recoveryReported_ || recoveryStopped_ != stopped || recoveryIssue_ != issue;
    recoveryReported_ = true;
    recoveryStopped_ = stopped;
    recoveryIssue_ = issue;
    lastReceipt_.insert("task_id", taskId_);
    lastReceipt_.insert("executor_lease_identity", lease_);
    lastReceipt_.insert("state", "needs_attention");
    lastReceipt_.insert("cleanup_status", "unverified");
    lastReceipt_.insert("containment_mode", "native_best_effort");
    lastReceipt_.insert("reason", "cleanup_unverified");
    lastReceipt_.insert("cleanup_issue", issue);
    lastReceipt_.insert("executor_stopped", stopped);
    lastReceipt_.insert("event", finished_ ? "update" : "done");
    finished_ = true;
    if (changed && event_)
        event_(lastReceipt_);
}
} // namespace ketplus::daemon::executor
#endif // !Q_OS_WIN
