#include "cloud/CloudEventProjection.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/BackgroundTaskSummary.h"
#include "runtime/ManagedTaskBridge.h"
#include "runtime/SessionHost.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>

namespace ketplus::daemon {
void ManagedTaskBridge::requestPermission(const QJsonObject& task, const QJsonObject& execution) {
    const auto id = task.value("task_id").toString();
    const BackgroundTaskOwner owner{
        task.value("owner_session_id").toString(), task.value("owner_generation").toInteger(), {}};
    const auto current = host_->sessions_.constFind(owner.sessionId);
    if (current == host_->sessions_.cend() || current->stored.generation != owner.generation) {
        runtime_->cancelOwner(owner, "owner_invalid");
        return;
    }
    const auto policy =
        task.value("execution").toObject().value("origin_execution_policy").toString();
    if (policy != "ask" && policy != "full") {
        // Missing snapshots (including pre-migration pending intents) cannot
        // borrow broader current-owner policy or be upgraded by manual approval.
        runtime_->permissionResolved(id, false);
        return;
    }
    if (policy == "full") {
        // The existing owner policy authorizes execution; this is not a grant
        // inherited from some earlier individual approval dialog.
        runtime_->permissionResolved(id, true);
        return;
    }
    const auto route =
        "managed-task-permission:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    permissions_.insert(route, {id, {}});
    auto preview = execution.value("shell_command").toString();
    if (preview.isEmpty())
        preview = QString::fromUtf8(
            QJsonDocument(execution.value("argv").toArray()).toJson(QJsonDocument::Compact));
    const auto environment = execution.value("env_delta").toObject();
    for (auto it = environment.begin(); it != environment.end(); ++it) {
        const auto value = it.value().toString();
        if (value.size() >= 4)
            preview.replace(value, "[redacted]");
    }
    const QJsonObject details{{"task_id", id},
                              {"title", task.value("title")},
                              {"cwd", execution.value("cwd")},
                              {"command", sanitizeCloudText(preview).left(4096)},
                              {"background_task", true}};
    const auto approval = host_->approvals_->request(owner.sessionId, owner.generation, route,
                                                     "approval", details, 0, true);
    if (permissions_.contains(route))
        permissions_[route].second = approval;
}

bool ManagedTaskBridge::permission(const QJsonValue& requestId, const QJsonObject& result) {
    const auto found = permissions_.find(requestId.toString());
    if (found == permissions_.end())
        return false;
    const auto id = found->first;
    permissions_.erase(found);
    const auto task = ledger_.runtimeTask(id);
    const BackgroundTaskOwner owner{
        task.value("owner_session_id").toString(), task.value("owner_generation").toInteger(), {}};
    const auto current = host_->sessions_.constFind(owner.sessionId);
    if (current == host_->sessions_.cend() || current->stored.generation != owner.generation)
        runtime_->cancelOwner(owner, "owner_invalid");
    else
        runtime_->permissionResolved(id, result.value("decision") == "accept" ||
                                             result.value("accepted").toBool() ||
                                             result.value("allow").toBool());
    return true;
}

void ManagedTaskBridge::hold(const BackgroundTaskOwner& owner, const QString& reason) {
    QString error;
    if (!ledger_.holdOwner(owner, reason, &error))
        qWarning("Unable to fence managed task wakes: %s", qUtf8Printable(error));
}

void ManagedTaskBridge::resume(const BackgroundTaskOwner& owner) {
    timer_.start();
    QString error;
    if (!ledger_.holdOwner(owner, {}, &error))
        qWarning("Unable to resume managed task wakes: %s", qUtf8Printable(error));
}

bool ManagedTaskBridge::hasExecutions() const {
    const auto page = ledger_.nonterminal();
    if (!page.value("ok").toBool())
        return true;
    for (const auto value : page.value("tasks").toArray()) {
        const auto task = value.toObject();
        if (task.value("state") != "needs_attention" || !task.value("executor_stopped").toBool())
            return true;
    }
    return false;
}

QJsonObject ManagedTaskBridge::snapshot(const BackgroundTaskOwner& owner) {
    auto page = ledger_.list(owner, {}, 16);
    if (!page.value("ok").toBool())
        return {};
    QJsonArray projected;
    QList<QJsonObject> active;
    const auto registry = ledger_.nonterminal();
    if (!registry.value("ok").toBool())
        return {};
    for (const auto value : registry.value("tasks").toArray()) {
        const auto task = value.toObject();
        if (task.value("owner_session_id") == owner.sessionId &&
            task.value("owner_generation").toInteger() == owner.generation)
            active.append(task);
    }
    std::stable_sort(active.begin(), active.end(), [](const auto& a, const auto& b) {
        return a.value("slot_held").toBool() && !b.value("slot_held").toBool();
    });
    for (auto task : active) {
        if (projected.size() == 16)
            break;
        task.insert("status", task.value("state"));
        projected.append(task);
    }
    const auto current = host_->sessions_.constFind(owner.sessionId);
    const bool owned =
        current != host_->sessions_.cend() && current->stored.generation == owner.generation;
    if (owned) {
        for (const auto value : current->backgroundTasks)
            if (projected.size() < 16)
                projected.append(value);
    }
    // The projection is bounded; the summary covers every job the owner's turn waits on.
    page.insert("summary",
                backgroundTaskSummary(active, owned ? current->backgroundTasks : QJsonArray{}));
    for (const auto value : page.value("tasks").toArray()) {
        auto task = value.toObject();
        if (projected.size() == 16)
            break;
        if (!task.value("ended_at").isDouble())
            continue;
        task.insert("status", task.value("state"));
        projected.append(task);
    }
    page.insert("tasks", projected);
    return page;
}

void ManagedTaskBridge::changed(const QJsonObject& task) {
    timer_.start();
    const auto id = task.value("task_id").toString();
    if (task.value("ended_at").isDouble())
        streamedItems_.remove(id);
    if (task.value("state") != "queued") {
        const auto routes = permissions_.keys();
        for (const auto& route : routes) {
            if (permissions_.value(route).first == id) {
                const auto approval = permissions_.value(route).second;
                host_->approvals_->cancelRequest(approval);
                permissions_.remove(route);
            }
        }
    }
    const auto owner = task.value("owner_session_id").toString();
    const auto generation = task.value("owner_generation").toInteger();
    auto current = host_->sessions_.find(owner);
    if (current == host_->sessions_.end() || current->stored.generation != generation)
        return;
    const auto page = snapshot({owner, generation, {}});
    host_->appendEvent(*current, "tasks.changed",
                       {{"tasks", page.value("tasks")},
                        {"summary", page.value("summary")},
                        {"revision", page.value("revision")},
                        {"active_count", page.value("active_count")}});
    if (task.value("state") == "needs_attention")
        host_->appendAgentNotice(owner, generation,
                                 {{"kind", "background_task_attention"},
                                  {"task_id", id},
                                  {"reason", task.value("reason")}});
}
} // namespace ketplus::daemon
