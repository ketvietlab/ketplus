#include "runtime/ManagedTaskBridge.h"
#include "providers/ProviderAdapter.h"
#include "runtime/ManagedTaskSpec.h"
#include "runtime/SessionHost.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

ManagedTaskBridge::ManagedTaskBridge(SessionHost* host)
    : QObject(host), host_(host), ledger_(host->journal_->backgroundTasks()) {
    connect(host_, &SessionHost::eventReady, this, [this](const QJsonObject& event) {
        recordChildEvent(event);
        const auto type = event.value("event_type").toString();
        if (type == "user.message" || type == "turn.completed" || type == "runtime.error" ||
            type == "approval.resolved" || type == "question.answered" || type == "session.deleted")
            timer_.start();
    });
    connect(host_, &SessionHost::teamEvent, this, [this] { timer_.start(); });
}

bool ManagedTaskBridge::initialize(QString* error) {
    if (!host_->journal_->isOpen()) {
        error_ = "The background registry's journal is unavailable.";
        *error = error_;
        return false;
    }
    auto directory = QFileInfo(host_->journal_->databasePath()).absoluteDir().canonicalPath();
    if (host_->journal_->databasePath() == ":memory:") {
        memoryDirectory_ = std::make_unique<QTemporaryDir>();
        directory = QFileInfo(memoryDirectory_->path()).canonicalFilePath();
    }
    runtime_ = std::make_unique<ManagedTaskRuntime>(
        ledger_, directory + "/background-jobs",
        QCoreApplication::applicationDirPath() + "/ketplus_job_executor", this);
    runtime_->setCallbacks([this](const QJsonObject& task) { changed(task); },
                           [this](const QJsonObject& task, const QJsonObject& execution) {
                               requestPermission(task, execution);
                           },
                           [this](const QJsonObject& task, const QJsonObject& execution) {
                               createChild(task, execution);
                           },
                           [this](const QJsonObject& task) { stopChild(task); });
    if (!runtime_->initialize(&error_)) {
        *error = error_;
        return false;
    }
    timer_.setInterval(500);
    const auto recoveredWakes = ledger_.recoverDispatchingWakes();
    if (!recoveredWakes.value("ok").toBool()) {
        error_ = recoveredWakes.value("error_message").toString();
        *error = error_;
        return false;
    }
    connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    timer_.start();
    return true;
}

void ManagedTaskBridge::command(const QJsonObject& request, const QString& clientId, Done done,
                                bool agentCaller, std::optional<AgentMutationGrant> origin) {
    const auto type = request.value("message_type").toString();
    if (type == "session.task.resolve_cleanup" && (agentCaller || clientId == "cloud-control-v2")) {
        done(
            failure("forbidden", "Cleanup acknowledgement requires the authenticated local user."));
        return;
    }
    if (!error_.isEmpty()) {
        done(failure("registry_unavailable", error_));
        return;
    }
    SessionHost::Session* session = nullptr;
    const auto error = host_->checkedSession(request, &session);
    // Existing local administration can inspect retained generations, including
    // cleanup incidents whose conversation has been deleted. Never grant this to
    // an agent credential or the remote control transport.
    const bool localAdministration = !agentCaller && clientId != "cloud-control-v2";
    if (!error.isEmpty() && (!localAdministration || type == "session.task.start" ||
                             request.value("session_id").toString().isEmpty() ||
                             request.value("session_generation").toInteger() <= 0)) {
        done(error);
        return;
    }
    if (!error.isEmpty())
        session = nullptr;
    const BackgroundTaskOwner owner =
        session ? BackgroundTaskOwner{session->stored.id, session->stored.generation,
                                      session->stored.turnId}
                : BackgroundTaskOwner{request.value("session_id").toString(),
                                      request.value("session_generation").toInteger(),
                                      {}};
    const auto payload = request.value("payload").toObject();
    if (agentCaller && (type == "session.task.start" || type == "session.task.stop")) {
        const auto authenticated =
            session && origin ? session->mutationOrigin.authorize(origin->credential,
                                                                  owner.sessionId, owner.generation)
                              : std::nullopt;
        if (!authenticated || session->stored.state != "running" || session->startingTurn ||
            session->stopRequested || session->interruptPending ||
            authenticated->turn != session->stored.currentTurnSequence) {
            done(failure("stale_origin", "The invocation mutation origin expired."));
            return;
        }
        origin->snapshot = *authenticated;
    }
    if (type == "session.task.start") {
        start(request, owner, std::move(done), std::move(origin));
        return;
    }
    if (type == "session.task.list") {
        done(ledger_.list(owner, payload.value("cursor").toString(),
                          payload.value("limit").toInt(50), localAdministration));
        return;
    }
    const auto id = payload.value("task_id").toString();
    const auto authorized = ledger_.get(id, owner, localAdministration);
    if (!authorized.value("ok").toBool()) {
        // Legacy snapshots remain a drain-only route, never registry authority.
        if ((type == "session.task.stop" || type == "session.task.output") &&
            authorized.value("error_code") == "task_not_found" && session) {
            for (const auto value : session->backgroundTasks) {
                if (value.toObject().value("task_id") == id) {
                    host_->backgroundTaskCommand(request, std::move(done));
                    return;
                }
            }
        }
        done(authorized);
        return;
    }
    if (type == "session.task.get")
        done(authorized);
    else if (type == "session.task.stop")
        done(runtime_->stop(id, owner, localAdministration));
    else if (type == "session.task.output")
        done(runtime_->output(id, owner, payload.value("cursor").toString(), localAdministration));
    else if (type == "session.task.resolve_cleanup")
        done(runtime_->resolveCleanup(id, payload.value("revision").toInteger(-1), clientId));
    else
        done(failure("unknown_command", "Unknown managed task operation."));
}

void ManagedTaskBridge::start(const QJsonObject& request, const BackgroundTaskOwner& owner,
                              Done done, std::optional<AgentMutationGrant> origin) {
    const auto payload = request.value("payload").toObject();
    BackgroundTaskIntent retryIntent;
    retryIntent.owner = owner;
    retryIntent.requestPayload = payload;
    retryIntent.clientRequestId = payload.value("client_request_id").toString();
    retryIntent.specFingerprint = QCryptographicHash::hash(
        QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    const auto retry = ledger_.retry(retryIntent);
    if (!retry.value("ok").toBool() || !retry.value("task").isNull()) {
        auto reply = retry;
        if (reply.value("ok").toBool())
            reply.insert("accepted", true);
        done(reply);
        return;
    }
    const auto current = host_->sessions_.constFind(owner.sessionId);
    if (current == host_->sessions_.cend() || current->stored.generation != owner.generation) {
        done(failure("stale_owner", "The task owner generation changed."));
        return;
    }
    const auto policy = origin ? origin->snapshot.executionPolicy : current->stored.executionPolicy;
    if (payload.value("kind") == "process" && policy == "read-only") {
        done(failure("permission_denied",
                     "Native process jobs cannot enforce this owner's read-only policy."));
        return;
    }
    const auto cwd = QFileInfo(host_->executionDirectory(*current)).canonicalFilePath();
    ManagedTaskContext context{owner,
                               cwd,
                               current->stored.adapter,
                               current->stored.model,
                               current->stored.effort,
                               [cwd](const QString& candidate) {
                                   return !cwd.isEmpty() &&
                                          (candidate == cwd || candidate.startsWith(cwd + '/'));
                               },
                               {}};
    context.executionPolicy = policy;
    auto submit = [this, owner, payload, context, done, origin](const QJsonArray& catalog) mutable {
        const auto current = host_->sessions_.constFind(owner.sessionId);
        if (current == host_->sessions_.cend() || current->stored.generation != owner.generation) {
            done(failure("stale_owner", "The task owner generation changed."));
            return;
        }
        // Catalog lookup may finish after completion/review/new-turn/Stop. A queued
        // request must still prove its original credential immediately before admission.
        if (origin && (!current->mutationOrigin.authorize(origin->credential, owner.sessionId,
                                                          owner.generation) ||
                       current->stored.state != "running" || current->startingTurn ||
                       current->stopRequested || current->interruptPending ||
                       current->stored.currentTurnSequence != origin->snapshot.turn)) {
            done(failure("stale_origin", "The originating turn ended before task acceptance."));
            return;
        }
        context.authorizeAgent = [catalog](const QString&, const QString& model,
                                           const QString& effort) {
            for (const auto value : catalog) {
                const auto candidate = value.toObject();
                if (candidate.value("id").toString() != model &&
                    candidate.value("model").toString() != model)
                    continue;
                if (effort.isEmpty())
                    return true;
                for (const auto value : candidate.value("efforts").toArray())
                    if (value.toObject().value("id") == effort)
                        return true;
            }
            return model.isEmpty() && effort.isEmpty() && !catalog.isEmpty();
        };
        const auto admitted = validateManagedTask(payload, context);
        if (!admitted.error.isEmpty())
            done(failure(admitted.errorCode, admitted.error));
        else {
            runtime_->start(admitted.intent, admitted.execution,
                            [done = std::move(done)](QJsonObject reply) {
                                if (reply.value("ok").toBool())
                                    reply.insert("accepted", true);
                                done(reply);
                            });
        }
    };
    if (payload.value("kind") != "agent") {
        submit({});
        return;
    }
    const auto selected =
        payload.value("execution_spec").toObject().value("provider").toString(context.provider);
    auto* target = host_->availableProvider(selected);
    if (!target) {
        done(failure("provider_unavailable", host_->missingProviderMessage(selected)));
        return;
    }
    host_->catalogCache_.get(
        selected, [target](auto callback) { target->configurationCatalog(std::move(callback)); },
        [guard = QPointer<ManagedTaskBridge>(this), submit, done](const QJsonObject& result,
                                                                  const QString& error) mutable {
            if (!guard)
                return;
            if (!error.isEmpty())
                done(failure("catalog_unavailable",
                             "The selected provider catalog is unavailable."));
            else
                submit(result.value("catalog").toArray());
        });
}
} // namespace ketplus::daemon
