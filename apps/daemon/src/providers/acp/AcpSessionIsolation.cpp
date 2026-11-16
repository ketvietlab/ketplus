#include "providers/acp/AcpAdapter.h"

#include <QProcess>
#include <QUuid>

namespace ketplus::daemon {
bool AcpAdapter::routeIsolatedCall(const QString& method, const QJsonObject& params,
                                   Callback& callback) {
    if (isolatedWorker_)
        return false;
    const auto session = params.value("sessionId").toString();
    if (method == "turn/interrupt" && sessionWorkers_.contains(session)) {
        if (auto worker = sessionWorkers_.value(session))
            worker->call(method, params, std::move(callback));
        else
            callback({}, "The session transport has exited.");
        return true;
    }
    const auto epoch = params.value("managedTransportEpoch").toString();
    if (method != "turn/start" || epoch.isEmpty())
        return false;
    if (session.isEmpty()) {
        callback({}, "mutation_refresh_unavailable: Missing transport session.");
        return true;
    }
    if (auto old = sessionWorkers_.value(session)) {
        if (!old->turns_.isEmpty() || !old->startingSessions_.isEmpty()) {
            callback({}, "A turn is already running for this session.");
            return true;
        }
        if (!releaseSessionTransport(session)) {
            callback({}, "mutation_refresh_unavailable: Previous ACP process did not exit.");
            return true;
        }
    }
    auto* worker = createSessionWorker();
    if (!worker) {
        callback({}, "mutation_refresh_unavailable: This adapter cannot isolate its transport.");
        return true;
    }
    worker->setParent(this);
    worker->isolatedWorker_ = true;
    worker->requireSessionLoad_ = !params.value("resume").toString().isEmpty();
    sessionWorkers_.insert(session, worker);
    connect(worker, &ProviderAdapter::canonicalEvent, this, &ProviderAdapter::canonicalEvent);
    connect(worker, &ProviderAdapter::requestReceived, this,
            [this, worker](const QJsonValue& id, const QString& method, const QJsonObject& params) {
                // ACP processes may reuse request IDs. A reply for a dead worker
                // must never answer an identically numbered request in its replacement.
                const auto route =
                    "isolated-acp:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
                isolatedRequests_.insert(route, {worker, id});
                emit requestReceived(route, method, params);
            });
    // A worker exit is local to its session; failPending publishes its canonical
    // failure. Forwarding stopped() would fence every session using this provider.
    worker->call(method, params, std::move(callback));
    return true;
}

bool AcpAdapter::releaseSessionTransport(const QString& session) {
    auto old = sessionWorkers_.value(session);
    if (!old) {
        sessionWorkers_.remove(session);
        return true;
    }
    if (!old->turns_.isEmpty() || !old->startingSessions_.isEmpty())
        return false;
    // Never stop the shared catalog process or another conversation.
    disconnect(old, nullptr, this, nullptr);
    old->stop();
    if (old->process_->state() != QProcess::NotRunning)
        return false;
    for (auto it = isolatedRequests_.begin(); it != isolatedRequests_.end();) {
        if (it->worker == old)
            it = isolatedRequests_.erase(it);
        else
            ++it;
    }
    sessionWorkers_.remove(session);
    delete old;
    return true;
}

bool AcpAdapter::respondIsolated(const QJsonValue& id, const QJsonObject& result,
                                 const QString& code, const QString& message) {
    const auto route = id.toString();
    if (!route.startsWith("isolated-acp:"))
        return false;
    const auto request = isolatedRequests_.take(route);
    if (request.worker) {
        if (code.isEmpty())
            request.worker->respond(request.originalId, result);
        else
            request.worker->respondError(request.originalId, code, message);
    }
    return true;
}

void AcpAdapter::stopSessionWorkers() {
    isolatedRequests_.clear();
    const auto workers = sessionWorkers_;
    for (auto worker : workers)
        if (worker)
            worker->stop();
}
} // namespace ketplus::daemon
