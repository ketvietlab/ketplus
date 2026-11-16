#include "runtime/executor/NativeJobClient.h"
#include "runtime/executor/NativeProcessIdentity.h"

#include <QJsonArray>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace ketplus::daemon::executor {
namespace {
NativeProcessIdentity fromJson(const QJsonObject& value) {
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
    const auto persisted = receipt();
    if (!persisted.isEmpty())
        lastReceipt_ = persisted;
    const auto state = lastReceipt_.value("state").toString();
    if (!state.isEmpty() && state != "needs_attention" && recoveryStarted_ < 0) {
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
