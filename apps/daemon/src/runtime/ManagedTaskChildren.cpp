#include "runtime/ApprovalBroker.h"
#include "runtime/ManagedTaskBridge.h"
#include "runtime/SessionHost.h"

#include <QJsonArray>

namespace ketplus::daemon {
QJsonObject ManagedTaskBridge::prepareTurn(const BackgroundTaskOwner& owner) {
    const auto linked = ledger_.linkedTask(owner.sessionId, owner.generation);
    if (!linked.value("ok").toBool())
        return linked;
    auto task = linked.value("task").toObject();
    if (task.isEmpty() || task.value("ended_at").isDouble())
        return {};
    const auto state = task.value("state").toString();
    if (state != "running" && state != "starting")
        return {{"ok", false},
                {"error_code", "session_busy"},
                {"error_message", "The managed child is being stopped."}};
    if (task.value("slot_held").toBool())
        return {};
    const auto id = task.value("task_id").toString();
    const auto resumed = runtime_->resumeChild(id);
    if (!resumed.value("ok").toBool())
        return resumed;
    task = ledger_.runtimeTask(id);
    if (!task.value("slot_held").toBool())
        return {{"ok", false},
                {"error_code", "resource_exhausted"},
                {"error_message", "The managed child is waiting for an execution slot."}};
    return {};
}

void ManagedTaskBridge::createChild(const QJsonObject& task, const QJsonObject& execution) {
    const auto id = task.value("task_id").toString();
    const BackgroundTaskOwner owner{
        task.value("owner_session_id").toString(), task.value("owner_generation").toInteger(), {}};
    const auto parent = host_->sessions_.constFind(owner.sessionId);
    if (parent == host_->sessions_.cend() || parent->stored.generation != owner.generation) {
        runtime_->cancelOwner(owner, "owner_invalid");
        return;
    }
    const auto policy =
        task.value("execution").toObject().value("origin_execution_policy").toString();
    if (policy != "ask" && policy != "full" && policy != "read-only") {
        (void)runtime_->stop(id, owner);
        return;
    }
    const auto reserved = runtime_->reserveChild(id);
    if (!reserved.value("ok").toBool())
        return;
    const auto identity = reserved.value("task").toObject().value("execution_identity").toObject();
    const auto childId = identity.value("child_session_id").toString();
    QString error;
    const auto created = host_->createMemberConversation(
        execution.value("cwd").toString(), execution.value("provider").toString(),
        execution.value("model").toString(), execution.value("effort").toString(),
        task.value("title").toString(), &error, policy, childId);
    if (created.isEmpty()) {
        BackgroundTaskChange failed;
        failed.state = "failed";
        failed.reason = "spawn_failed";
        failed.neverLaunched = true;
        runtime_->childChanged(id, failed);
        return;
    }
    auto child = host_->sessions_.find(created);
    if (child == host_->sessions_.end())
        return;
    host_->appendEvent(*child, "task.child_linked",
                       {{"parent_task_id", id},
                        {"owner_session_id", owner.sessionId},
                        {"owner_generation", owner.generation}});
    const auto failure =
        host_->startAgentTurn(created, identity.value("child_generation").toInteger(),
                              execution.value("prompt").toString(),
                              {{"kind", "managed_background_child"}, {"parent_task_id", id}});
    if (!failure.isEmpty()) {
        BackgroundTaskChange stopping;
        stopping.state = "stopping";
        stopping.terminalTarget = "failed";
        stopping.reason = "spawn_failed";
        runtime_->childChanged(id, stopping);
        stopChild(ledger_.runtimeTask(id));
        return;
    }
    BackgroundTaskChange running;
    running.state = "running";
    running.handshakeCommitted = true;
    running.executionIdentity = identity;
    runtime_->childChanged(id, running);
}

void ManagedTaskBridge::stopChild(const QJsonObject& task) {
    const auto id = task.value("task_id").toString();
    const auto identity = task.value("execution_identity").toObject();
    const auto childId = identity.value("child_session_id").toString();
    const auto generation = identity.value("child_generation").toInteger();
    const auto target = task.value("state") == "stopping"
                            ? task.value("terminal_target").toString("cancelled")
                            : QStringLiteral("interrupted");
    stoppingChildren_.insert(id, target);
    if (childId.isEmpty()) {
        BackgroundTaskChange stopped;
        stopped.state = target;
        stopped.cleanupConfirmed = true;
        stopped.neverLaunched = true;
        runtime_->childChanged(id, stopped);
        return;
    }
    runtime_->cancelOwner({childId, generation, {}}, "parent_job_stopped");
    host_->approvals_->cancelPending(childId, generation);
    const auto child = host_->sessions_.constFind(childId);
    if (child == host_->sessions_.cend() || child->stored.generation != generation)
        return;
    if (child->startingTurn || child->stored.state == "starting" ||
        child->stored.state == "running" || child->stored.state == "waiting_for_input")
        (void)host_->interrupt({{"session_id", childId}, {"session_generation", generation}});
}

void ManagedTaskBridge::childOutput(const QString& child, qint64 generation, qint64 turn,
                                    const QString& text) {
    const auto linked = ledger_.linkedTask(child, generation).value("task").toObject();
    if (!linked.isEmpty() && linked.value("state") != "completed" &&
        linked.value("state") != "failed" && linked.value("state") != "cancelled" &&
        linked.value("state") != "timed_out" && linked.value("state") != "interrupted")
        if (!runtime_->appendChildOutput(linked.value("task_id").toString(), turn, text.toUtf8()))
            qWarning("Unable to persist managed child output.");
}

void ManagedTaskBridge::pollChildren() {
    for (const auto value : ledger_.nonterminal().value("tasks").toArray()) {
        auto task = value.toObject();
        if (task.value("kind") != "agent")
            continue;
        const auto id = task.value("task_id").toString();
        const auto identity = task.value("execution_identity").toObject();
        const auto childId = identity.value("child_session_id").toString();
        if (childId.isEmpty())
            continue;
        const auto child = host_->sessions_.constFind(childId);
        if (child != host_->sessions_.cend() &&
            child->stored.generation == identity.value("child_generation").toInteger() &&
            !child->failedTurnId.isEmpty() && child->failedTurnId == child->stored.turnId &&
            task.value("state") != "stopping" && !stoppingChildren_.contains(id)) {
            BackgroundTaskChange stopping;
            stopping.state = "stopping";
            stopping.terminalTarget = "failed";
            stopping.reason = "child_failed";
            runtime_->childChanged(id, stopping);
            task = ledger_.runtimeTask(id);
            if (task.value("state") == "stopping")
                stopChild(task);
            // Await provider completion/cancellation evidence before terminal.
        }
        if (child == host_->sessions_.cend()) {
            if (host_->journal_->sessionExists(childId))
                continue; // Missing runtime state is not cleanup evidence.
        } else if (child->stored.generation != identity.value("child_generation").toInteger()) {
            if (!task.value("cleanup_pending").toBool()) {
                BackgroundTaskChange pending;
                pending.state = "stopping";
                pending.terminalTarget = task.value("terminal_target").toString("interrupted");
                pending.reason = "child_generation_changed_cleanup_pending";
                pending.cleanupPending = true;
                runtime_->childChanged(id, pending);
            }
            continue; // Never interrupt or adopt the replacement generation.
        } else if (child->startingTurn || child->stored.state == "starting" ||
                   child->stored.state == "running" || child->stored.state == "waiting_for_input" ||
                   child->stored.state == "stopping") {
            continue;
        }
        if (task.value("state") == "starting" && child != host_->sessions_.cend() &&
            child->stored.currentTurnSequence > 0 && !stoppingChildren_.contains(id)) {
            BackgroundTaskChange running;
            running.state = "running";
            running.executionIdentity = identity;
            running.handshakeCommitted = true;
            runtime_->childChanged(id, running);
            task = ledger_.runtimeTask(id);
        }
        if (!stoppingChildren_.contains(id) && task.value("state") != "stopping" &&
            (child == host_->sessions_.cend() || child->stored.state == "failed" ||
             child->stored.state == "interrupted")) {
            // Fence descendant completions before cancelling their ownership tree.
            // A failed child must never be restarted by one of its own job results.
            BackgroundTaskChange stopping;
            stopping.state = "stopping";
            stopping.terminalTarget = child == host_->sessions_.cend() ? "interrupted" : "failed";
            stopping.reason =
                child == host_->sessions_.cend() ? "child_executor_lost" : "child_failed";
            runtime_->childChanged(id, stopping);
            task = ledger_.runtimeTask(id);
            if (task.value("state") == "stopping") {
                stopChild(task);
            }
        }
        if (child != host_->sessions_.cend() && child->stored.awaitingReply &&
            !stoppingChildren_.contains(id)) {
            (void)runtime_->parkChild(id); // The turn ended; no inference is running.
            continue;
        }
        if (child != host_->sessions_.cend() &&
            (!child->deferredJournal.isEmpty() || !child->pendingDeltaType.isEmpty()))
            continue;
        if (child != host_->sessions_.cend() &&
            !host_->approvals_->pendingPage(childId, child->stored.generation)
                 .value("pending_approvals")
                 .toArray()
                 .isEmpty())
            continue;
        BackgroundTaskChange done;
        done.state = stoppingChildren_.value(
            id, task.value("state") == "stopping"
                    ? task.value("terminal_target").toString("cancelled")
                : child == host_->sessions_.cend() ? "interrupted"
                : child->stored.state == "failed" || child->stored.state == "interrupted"
                    ? "failed"
                    : "completed");
        done.reason =
            done.state == "failed" ? task.value("reason").toString("child_failed") : QString{};
        done.cleanupConfirmed = true;
        done.neverLaunched =
            child == host_->sessions_.cend() || child->stored.currentTurnSequence == 0;
        done.summary = "Managed child task " + id + " ended with state " + done.state + '.';
        const BackgroundTaskOwner owner{task.value("owner_session_id").toString(),
                                        task.value("owner_generation").toInteger(),
                                        {}};
        const auto output = runtime_->output(id, owner, {}, true);
        done.outputSize = output.value("total_bytes").toInteger();
        done.outputTruncated = output.value("truncated").toBool();
        runtime_->childChanged(id, done);
        const auto remaining = ledger_.runtimeTask(id);
        if (remaining.value("state") == "running")
            (void)runtime_->parkChild(
                id); // Waiting for descendants and their completion continuations.
        else if (remaining.value("state") != "starting" && remaining.value("state") != "stopping")
            stoppingChildren_.remove(id);
    }
}
} // namespace ketplus::daemon
