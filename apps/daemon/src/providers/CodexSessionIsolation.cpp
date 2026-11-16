#include "agents/AgentToolLaunch.h"
#include "providers/CodexAdapter.h"

#include <QJsonArray>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <memory>
#include <utility>

namespace ketplus::daemon {
bool CodexAdapter::routeIsolatedCall(const QString& method, const QJsonObject& params,
                                     Callback& callback) {
    if (isolatedWorker_)
        return false;
    const auto session = params.value("managedSessionId").toString();
    const auto epoch = params.value("managedTransportEpoch").toString();
    if ((method != "thread/start" && method != "thread/resume") || epoch.isEmpty()) {
        const auto owner = sessionByThread_.value(params.value("threadId").toString());
        if (owner.isEmpty())
            return false;
        if (auto worker = sessionWorkers_.value(owner).adapter)
            worker->call(method, params, std::move(callback));
        else
            callback({}, "mutation_refresh_unavailable: The session app-server exited.");
        return true;
    }
    if (session.isEmpty()) {
        callback({}, "mutation_refresh_unavailable: Missing Codex transport session.");
        return true;
    }
    if (const auto old = sessionWorkers_.value(session); old.adapter) {
        if (old.preparing || !old.adapter->activeTurns_.isEmpty()) {
            callback({}, "A Codex turn is already running for this session.");
            return true;
        }
        if (!releaseSessionTransport(session)) {
            callback({}, "mutation_refresh_unavailable: Previous Codex process did not exit.");
            return true;
        }
    }
    auto* worker = new CodexAdapter(executable(), this);
    worker->isolatedWorker_ = true;
    worker->setBrowserArgumentsProvider(browserArgumentsProvider_);
    const auto agentEnvironment =
        params.value("config").toObject().value("mcp_servers.ketplus_agents.env").toObject();
    const bool mutationExpected =
        !agentEnvironment.value(QLatin1String(agent_tools::mutationVariable)).toString().isEmpty();
    sessionWorkers_.insert(session, {worker, epoch, {}, {}, true, mutationExpected});
    connect(worker, &CodexAdapter::notificationReceived, this,
            [this, session, epoch](const QString& method, const QJsonObject& params) {
                auto current = sessionWorkers_.find(session);
                if (current == sessionWorkers_.end() || current->epoch != epoch)
                    return;
                if (method == "turn/started")
                    current->turnId = params.value("turn").toObject().value("id").toString();
                else if (method == "turn/completed")
                    current->turnId.clear();
                emit notificationReceived(method, params);
            });
    connect(worker, &CodexAdapter::notificationSkipped, this, &CodexAdapter::notificationSkipped);
    connect(worker, &ProviderAdapter::requestReceived, this,
            [this, worker](const QJsonValue& id, const QString& method, const QJsonObject& params) {
                const auto route =
                    "isolated-codex:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                isolatedRequests_.insert(route, {worker, id});
                emit requestReceived(route, method, params);
            });
    connect(worker, &ProviderAdapter::stopped, this, [this, session, epoch](const QString& error) {
        const auto current = sessionWorkers_.value(session);
        if (current.epoch != epoch || current.preparing || current.threadId.isEmpty())
            return;
        emit notificationReceived("error", {{"threadId", current.threadId},
                                            {"turnId", current.turnId},
                                            {"willRetry", false},
                                            {"error", QJsonObject{{"message", error}}}});
        if (!current.turnId.isEmpty())
            emit notificationReceived(
                "turn/completed",
                {{"threadId", current.threadId},
                 {"turn", QJsonObject{{"id", current.turnId}, {"status", "failed"}}}});
    });
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    const auto settled = std::make_shared<bool>(false);
    Callback finish = [this, session, epoch, timer, settled, callback = std::move(callback)](
                          const QJsonObject& result, const QString& error) {
        if (std::exchange(*settled, true))
            return;
        timer->stop();
        timer->deleteLater();
        auto current = sessionWorkers_.find(session);
        if (current == sessionWorkers_.end() || current->epoch != epoch)
            return;
        current->preparing = false;
        if (!error.isEmpty()) {
            if (current->adapter) {
                disconnect(current->adapter, nullptr, this, nullptr);
                current->adapter->stop();
            }
            callback({}, "mutation_refresh_unavailable: Codex context refresh failed: " + error);
        } else {
            auto acknowledged = result;
            acknowledged.insert("managedTransportEpoch", epoch);
            callback(acknowledged, {});
        }
    };
    connect(timer, &QTimer::timeout, this, [finish] { finish({}, "Timed out."); });
    timer->start(originRefreshTimeoutMs_);
    auto setup = params;
    setup.remove("managedSessionId");
    setup.remove("managedTransportEpoch");
    const auto expected =
        method == "thread/resume" ? setup.value("threadId").toString() : QString{};
    worker->call(
        method, setup,
        [this, session, epoch, expected, finish](const QJsonObject& result, const QString& error) {
            auto current = sessionWorkers_.find(session);
            if (current == sessionWorkers_.end() || current->epoch != epoch || !current->preparing)
                return;
            if (!error.isEmpty()) {
                finish({}, error);
                return;
            }
            const auto thread = result.value("thread").toObject().value("id").toString();
            if (thread.isEmpty() || (!expected.isEmpty() && thread != expected)) {
                finish({}, "The provider did not restore the requested thread.");
                return;
            }
            current->threadId = thread;
            sessionByThread_.insert(thread, session);
            confirmIsolatedMcp(session, epoch, result, finish);
        });
    return true;
}

void CodexAdapter::confirmIsolatedMcp(const QString& session, const QString& epoch,
                                      const QJsonObject& setup, Callback done) {
    const auto current = sessionWorkers_.value(session);
    if (current.epoch != epoch || !current.preparing || !current.adapter)
        return;
    current.adapter->call(
        "mcpServerStatus/list", {{"threadId", current.threadId}, {"limit", 100}},
        [this, session, epoch, setup, done](const QJsonObject& status, const QString& error) {
            const auto current = sessionWorkers_.value(session);
            if (current.epoch != epoch || !current.preparing)
                return;
            if (!error.isEmpty()) {
                done({}, error);
                return;
            }
            for (const auto value : status.value("data").toArray()) {
                const auto server = value.toObject();
                if (server.value("name") != "ketplus_agents")
                    continue;
                if (!server.value("toolsError").toString().isEmpty()) {
                    done({}, "The agent MCP transport failed discovery.");
                    return;
                }
                const auto tools = server.value("tools").toObject();
                const bool readReady = tools.contains("agents_whoami") &&
                                       tools.contains("background_task_list") &&
                                       tools.contains("background_task_get") &&
                                       tools.contains("background_task_output");
                const bool mutationReady =
                    !current.mutationExpected || (tools.contains("background_task_start") &&
                                                  tools.contains("background_task_stop"));
                if (readReady && mutationReady) {
                    done(setup, {});
                    return;
                }
            }
            QTimer::singleShot(50, this, [this, session, epoch, setup, done] {
                confirmIsolatedMcp(session, epoch, setup, done);
            });
        });
}

bool CodexAdapter::releaseSessionTransport(const QString& session) {
    const auto old = sessionWorkers_.value(session);
    if (!old.adapter) {
        sessionWorkers_.remove(session);
        return true;
    }
    if (old.preparing || !old.adapter->activeTurns_.isEmpty())
        return false;
    disconnect(old.adapter, nullptr, this, nullptr);
    old.adapter->stop();
    if (old.adapter->process_->state() != QProcess::NotRunning)
        return false;
    sessionByThread_.remove(old.threadId);
    for (auto it = isolatedRequests_.begin(); it != isolatedRequests_.end();) {
        if (it->adapter == old.adapter)
            it = isolatedRequests_.erase(it);
        else
            ++it;
    }
    sessionWorkers_.remove(session);
    delete old.adapter;
    return true;
}

bool CodexAdapter::respondIsolated(const QJsonValue& id, const QJsonObject& result,
                                   const QString& code, const QString& message) {
    const auto route = id.toString();
    if (!route.startsWith("isolated-codex:"))
        return false;
    const auto request = isolatedRequests_.take(route);
    if (request.adapter) {
        if (code.isEmpty())
            request.adapter->respond(request.originalId, result);
        else
            request.adapter->respondError(request.originalId, code, message);
    }
    return true;
}
bool CodexAdapter::isolatedWorkersIdle() const {
    for (const auto& worker : sessionWorkers_)
        if (worker.preparing || (worker.adapter && !worker.adapter->isIdle()))
            return false;
    return true;
}
void CodexAdapter::stopSessionWorkers() {
    isolatedRequests_.clear();
    const auto workers = sessionWorkers_;
    for (const auto& worker : workers)
        if (worker.adapter)
            worker.adapter->stop();
}
} // namespace ketplus::daemon
