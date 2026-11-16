#include "agent/AgentSessionManager.h"
#include "agent/AgentSessionState.h"
#include "agent/CodexAppServer.h"

#include <QJsonArray>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <utility>

namespace ketplus {
namespace {
QString state(const QVariantMap& task) {
    return task.value("state", task.value("status")).toString();
}
bool terminal(const QVariantMap& task) {
    static const QSet<QString> states{"completed", "failed",    "cancelled",
                                      "stopped",   "timed_out", "interrupted"};
    return states.contains(state(task));
}
QVariantList merge(const QVariantList& base, const QVariantList& incoming) {
    QVariantList tasks = base;
    for (const auto& value : incoming) {
        auto task = value.toMap();
        task.insert("status", state(task));
        if (task.contains("revision"))
            task.insert("revisionToken", QString::number(task.value("revision").toLongLong()));
        auto found = std::find_if(tasks.begin(), tasks.end(), [&task](const auto& entry) {
            return entry.toMap().value("task_id") == task.value("task_id");
        });
        if (found == tasks.end())
            tasks.append(task);
        else if (found->toMap().value("revision").toLongLong() <=
                 task.value("revision").toLongLong()) {
            const auto previous = found->toMap().value("completion").toMap();
            if (previous.value("status") == "delivered" || previous.value("status") == "suppressed")
                task.insert("completion", previous);
            *found = task;
        }
    }
    std::stable_sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) {
        return !terminal(a.toMap()) && terminal(b.toMap());
    });
    return tasks;
}
} // namespace

QVariantMap AgentSessionManager::backgroundTaskState(const QString& worktreePath) const {
    const auto session = sessions_.value(normalizedPath(worktreePath));
    auto result = server_->backgroundTaskAccess();
    if (session) {
        for (auto it = session->backgroundTaskPage.cbegin();
             it != session->backgroundTaskPage.cend(); ++it)
            result.insert(it.key(), it.value());
        result.insert("scope",
                      session->threadId + ":" + QString::number(session->daemonGeneration));
        result.insert("loading", session->backgroundTaskLoading);
    }
    return result;
}

void AgentSessionManager::loadBackgroundTasks(const QString& worktreePath, bool more) {
    const auto path = normalizedPath(worktreePath);
    const auto session = sessions_.value(path);
    if (!session || session->threadId.isEmpty() || !server_->usesDaemon() || !server_->isReady())
        return;
    if (session->backgroundTaskLoading) {
        if (!more)
            session->backgroundTaskRefreshAgain = true;
        return;
    }
    const auto cursor =
        more ? session->backgroundTaskPage.value("nextCursor").toString() : QString{};
    if (more && cursor.isEmpty())
        return;
    session->backgroundTaskLoading = true;
    session->backgroundTaskPage.remove("error");
    emitBackgroundTasks(session);
    const auto owner = session->threadId;
    const auto generation = session->daemonGeneration;
    server_->call(
        "task/list", {{"threadId", owner}, {"cursor", cursor}, {"limit", 100}},
        [this, guard = QPointer<AgentSessionManager>(this), session, path, owner,
         generation](const QJsonObject& reply, const QString& error) {
            if (!guard || sessionForThread(owner) != session || session->threadId != owner ||
                session->daemonGeneration != generation)
                return;
            session->backgroundTaskLoading = false;
            QString message = error;
            if (message.isEmpty() && !reply.value("ok").toBool())
                message =
                    reply.value("error_message").toString("Unable to read the task registry.");
            if (!message.isEmpty()) {
                session->backgroundTaskPage.insert("error", message);
                if (reply.value("error_code") == "stale_cursor" &&
                    session->backgroundTaskCursorRetries++ < 2)
                    session->backgroundTaskRefreshAgain = true;
            } else {
                session->backgroundTaskCursorRetries = 0;
                session->backgroundTasks =
                    merge(merge(session->backgroundTasks, session->backgroundTaskProjection),
                          reply.value("tasks").toArray().toVariantList());
                session->backgroundTaskPage = {
                    {"nextCursor", reply.value("next_cursor").toString()},
                    {"revision", reply.value("revision").toVariant()},
                    {"activeCount", reply.value("active_count").toInt()}};
            }
            emitBackgroundTasks(session);
            if (isActiveSession(session) && message.isEmpty())
                refreshBackgroundTaskMetadata(session);
            if (std::exchange(session->backgroundTaskRefreshAgain, false) &&
                isActiveSession(session))
                loadBackgroundTasks(path, false);
        });
}

void AgentSessionManager::updateBackgroundTaskProjection(const SessionPtr& session,
                                                         const QVariantList& tasks) {
    session->backgroundTaskProjection = tasks;
    auto managed = session->backgroundTasks;
    managed.removeIf([](const auto& value) { return !value.toMap().contains("schema_version"); });
    session->backgroundTasks = merge(managed, tasks);
    emitBackgroundTasks(session);
    if (!isActiveSession(session))
        return;
    loadBackgroundTasks(session->path, false);
    refreshBackgroundTaskMetadata(session);
}

void AgentSessionManager::refreshBackgroundTaskMetadata(const SessionPtr& session) {
    QSet<QString> projected;
    for (const auto& value : session->backgroundTaskProjection)
        projected.insert(value.toMap().value("task_id").toString());
    // Absence from a 16-row projection is not deletion or a terminal result.
    // Reconcile known nonterminal records outside it with authoritative get.
    const auto known = session->backgroundTasks;
    for (const auto& value : known) {
        const auto task = value.toMap();
        const auto id = task.value("task_id").toString();
        if (terminal(task) || !task.contains("schema_version") || projected.contains(id) ||
            session->backgroundTaskMetadataReads.contains(id))
            continue;
        session->backgroundTaskMetadataReads.insert(id);
        const auto owner = session->threadId;
        const auto generation = session->daemonGeneration;
        server_->call(
            "task/get", {{"threadId", owner}, {"taskId", id}},
            [this, guard = QPointer<AgentSessionManager>(this), session, owner, generation,
             id](const QJsonObject& reply, const QString& error) {
                if (!guard || sessionForThread(owner) != session ||
                    session->daemonGeneration != generation)
                    return;
                session->backgroundTaskMetadataReads.remove(id);
                const auto record = reply.value("task").toObject();
                if (error.isEmpty() && reply.value("ok").toBool() && record.value("task_id") == id)
                    session->backgroundTasks =
                        merge(session->backgroundTasks, {record.toVariantMap()});
                else if (reply.value("error_code") == "task_not_found" ||
                         reply.value("error_code") == "task_expired")
                    session->backgroundTasks.removeIf([&id](const auto& value) {
                        return value.toMap().value("task_id").toString() == id;
                    });
                else if (!error.isEmpty() || !reply.value("ok").toBool())
                    session->backgroundTaskPage.insert(
                        "error", error.isEmpty() ? reply.value("error_message")
                                                       .toString("Unable to refresh task metadata.")
                                                 : error);
                emitBackgroundTasks(session);
            });
    }
}

void AgentSessionManager::stopBackgroundTask(const QString& path, const QString& id) {
    callBackgroundTask(path, id, "stop");
}
void AgentSessionManager::resolveBackgroundTaskCleanup(const QString& path, const QString& id,
                                                       qint64 revision) {
    callBackgroundTask(path, id, "resolve_cleanup", {{"revision", revision}});
}
void AgentSessionManager::readBackgroundTaskOutput(const QString& path, const QString& id,
                                                   const QString& cursor, quint64 request) {
    callBackgroundTask(path, id, "output",
                       {{"cursor", cursor}, {"request", QString::number(request)}});
}

void AgentSessionManager::callBackgroundTask(const QString& worktreePath, const QString& taskId,
                                             const QString& action, const QJsonObject& parameters) {
    const auto path = normalizedPath(worktreePath);
    const auto session = sessions_.value(path);
    if (!session || session->threadId.isEmpty() || !server_->usesDaemon()) {
        emit backgroundTaskResult(
            path, taskId, action,
            {{"error", "This conversation has no background task connection."},
             {"request", parameters.value("request").toVariant()}});
        return;
    }
    const auto owner = session->threadId;
    const auto generation = session->daemonGeneration;
    auto params = parameters;
    params.insert("threadId", owner);
    params.insert("taskId", taskId);
    server_->call(
        "task/" + action, params,
        [this, guard = QPointer<AgentSessionManager>(this), session, owner, generation, path,
         taskId, action, parameters](const QJsonObject& reply, const QString& error) {
            if (!guard || sessionForThread(owner) != session || session->threadId != owner ||
                session->daemonGeneration != generation)
                return;
            auto result = reply.toVariantMap();
            QString message = error;
            if (message.isEmpty() && !reply.value("ok").toBool(true))
                message = reply.value("error_message").toString("The task operation failed.");
            if (!message.isEmpty())
                result.insert("error", message);
            result.insert("request", parameters.value("request").toVariant());
            if (isActiveSession(session))
                emit backgroundTaskResult(path, taskId, action, result);
            if (action != "output" && isActiveSession(session))
                loadBackgroundTasks(path, false);
        });
}
} // namespace ketplus
