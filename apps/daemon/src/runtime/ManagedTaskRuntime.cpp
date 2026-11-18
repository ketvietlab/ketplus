#include "runtime/ManagedTaskRuntime.h"
#include "runtime/executor/LinuxJobContainment.h"
#include "runtime/executor/NativeJobClient.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QScopedValueRollback>
#include <QSet>
#include <QUuid>
#if !defined(Q_OS_WIN)
#include <unistd.h>
#endif

namespace ketplus::daemon {
namespace {
// QFileInfo::ownerId() is -2 on Windows, where background jobs are not available anyway.
uint effectiveUserId() {
#if defined(Q_OS_WIN)
    return static_cast<uint>(-2);
#else
    return ::geteuid();
#endif
}
} // namespace
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
bool terminal(const QString& state) {
    return state == "completed" || state == "failed" || state == "cancelled" ||
           state == "timed_out" || state == "interrupted";
}
} // namespace

ManagedTaskRuntime::ManagedTaskRuntime(BackgroundTaskLedger ledger, QString directory,
                                       QString executor, QObject* parent)
    : QObject(parent), ledger_(std::move(ledger)), directory_(std::move(directory)),
      executor_(std::move(executor)) {
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, [this] { pump(); });
    maintenance_.setInterval(60 * 60 * 1000);
    connect(&maintenance_, &QTimer::timeout, this, [this] { maintain(); });
}

void ManagedTaskRuntime::setCallbacks(Changed changed, Permission permission, ChildStart childStart,
                                      ChildStop childStop) {
    changed_ = std::move(changed);
    permission_ = std::move(permission);
    childStart_ = std::move(childStart);
    childStop_ = std::move(childStop);
}

bool ManagedTaskRuntime::initialize(QString* error) {
    const QFileInfo existing(directory_);
    if (existing.exists() && (existing.isSymLink() || existing.canonicalFilePath() != directory_ ||
                              existing.ownerId() != effectiveUserId())) {
        if (error)
            *error = "The existing background directory has an unexpected path or owner.";
        return false;
    }
    if (!QDir().mkpath(directory_) ||
        !QFile::setPermissions(directory_,
                               QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        if (error)
            *error = "Unable to create the private background execution directory.";
        return false;
    }
    const QFileInfo root(directory_);
    if (root.isSymLink() || root.canonicalFilePath() != directory_ ||
        root.ownerId() != effectiveUserId()) {
        if (error)
            *error = "The background execution directory must be canonical and owner-controlled.";
        return false;
    }
    const auto recovered = ledger_.nonterminal();
    if (!recovered.value("ok").toBool()) {
        if (error)
            *error = recovered.value("error_message").toString();
        return false;
    }
    for (const auto value : recovered.value("tasks").toArray()) {
        auto live = std::make_shared<LiveTask>();
        live->record = value.toObject();
        const auto id = live->record.value("task_id").toString();
        live_.insert(id, live);
    }
    // Defer callbacks until SessionHost has finished constructing and attached its ports.
    const auto recoveredIds = live_.keys();
    QTimer::singleShot(0, this, [this, recoveredIds] {
        for (const auto& id : recoveredIds) {
            const auto live = live_.value(id);
            if (!live)
                continue;
            if (live->record.value("kind") == "process") {
                reconcileProcess(id);
            } else if (childStop_) {
                // Stop the linked child through the provider pipeline. Its inference
                // may have started before the crash; never submit its prompt again.
                childStop_(live->record);
            }
        }
        if (!live_.isEmpty())
            timer_.start();
        pump();
        maintain();
        maintenance_.start();
    });
    return true;
}

QJsonObject ManagedTaskRuntime::resolveCleanup(const QString& id, qint64 revision,
                                               const QString& clientId) {
    const auto result = ledger_.resolveCleanup(id, revision, clientId);
    if (result.value("ok").toBool()) {
        live_.remove(id);
        if (live_.isEmpty())
            timer_.stop();
        if (changed_)
            changed_(result.value("task").toObject());
    }
    return result;
}

void ManagedTaskRuntime::maintain() {
    const auto result = ledger_.prune(QDateTime::currentMSecsSinceEpoch());
    if (!result.value("ok").toBool()) {
        qWarning("Unable to prune background task metadata.");
        return;
    }
    const auto cleanup = result.value("output_cleanup").toArray();
    int removed = 0;
    for (const auto& value : cleanup) {
        const auto id = value.toString();
        // Task identities are generated by the registry, never supplied paths.
        if (QUuid(id).isNull())
            continue;
        const auto path = directory_ + '/' + id;
        const QFileInfo file(path);
        if (file.isSymLink())
            continue;
        if (!file.exists() ||
            (file.isDir() && file.canonicalFilePath() == path &&
             file.ownerId() == effectiveUserId() && QDir(path).removeRecursively()))
            if (ledger_.outputRemoved(id))
                ++removed;
    }
    // Drain an old backlog in bounded transactions without waiting an hour for
    // each 100 jobs. Yield between batches so provider/IPC work can proceed.
    if (result.value("pruned").toInt() == 100 || (cleanup.size() == 100 && removed > 0))
        QTimer::singleShot(100, this, [this] { maintain(); });
}

void ManagedTaskRuntime::start(const BackgroundTaskIntent& intent, const QJsonObject& execution,
                               Done done) {
    const auto retry = ledger_.retry(intent);
    if (!retry.value("ok").toBool() || !retry.value("task").isNull()) {
        done(retry);
        return;
    }
    if (intent.kind == "process") {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_LINUX)
        done(failure("containment_unavailable", "A verified process executor is required."));
        return;
#endif
        if (!QFileInfo(executor_).isExecutable()) {
            done(failure("executor_unavailable",
                         "The managed background executor is not installed."));
            return;
        }
    } else if (!childStart_ || !childStop_) {
        done(failure("executor_unavailable", "The managed child executor is unavailable."));
        return;
    }
    if (!permission_) {
        done(failure("permission_unavailable", "The background permission broker is unavailable."));
        return;
    }
#ifdef Q_OS_LINUX
    if (intent.kind == "process") {
        executor::linuxContainmentAvailableAsync(
            executor_, this,
            [this, intent, execution, done = std::move(done)](bool available, QString error) {
                if (!available)
                    done(failure("containment_unavailable", error));
                else
                    done(accept(intent, execution));
            });
        return;
    }
#endif
    done(accept(intent, execution));
}

QJsonObject ManagedTaskRuntime::accept(const BackgroundTaskIntent& intent,
                                       const QJsonObject& execution) {
    // create() rechecks the current generation, quotas and retry key after
    // asynchronous admission; no workload/permission is launched by the probe.
    auto acceptedIntent = intent;
#ifdef Q_OS_LINUX
    if (intent.kind == "process")
        acceptedIntent.containmentMode = "cgroup_v2";
#endif
    const auto reply = ledger_.create(acceptedIntent);
    if (!reply.value("ok").toBool() || reply.value("reused").toBool())
        return reply;
    const auto id = reply.value("task").toObject().value("task_id").toString();
    auto live = std::make_shared<LiveTask>();
    live->record = reply.value("task").toObject();
    live->execution = execution;
    live_.insert(id, live);
    timer_.start();
    // The durable accepted reply exists before requesting approval or spawning work.
    QTimer::singleShot(0, this, [this, id] {
        const auto live = live_.value(id);
        if (!live || live->record.value("state") != "queued")
            return;
        publish(id);
        permission_(live->record, live->execution);
    });
    return reply;
}

bool ManagedTaskRuntime::apply(const QString& id, const BackgroundTaskChange& change) {
    const auto live = live_.value(id);
    if (!live)
        return false;
    const auto reply = ledger_.change(id, live->record.value("revision").toInteger(), change);
    live->persistenceError = reply.value("error_code").toString();
    if (!reply.value("ok").toBool())
        return false; // Preserve ownership; a failed persistence operation cannot declare cleanup.
    live->record = reply.value("task").toObject();
    publish(id);
    if (terminal(live->record.value("state").toString())) {
        live->execution = {};
        if (live->executor) {
            live->executor->acknowledge();
            live->executor->deleteLater();
        }
        live_.remove(id);
        if (live_.isEmpty())
            timer_.stop();
    }
    return true;
}

void ManagedTaskRuntime::publish(const QString& id) {
    const auto live = live_.value(id);
    if (live && changed_)
        changed_(live->record);
}

void ManagedTaskRuntime::permissionResolved(const QString& id, bool granted) {
    const auto live = live_.value(id);
    if (!live || live->record.value("state") != "queued")
        return;
    BackgroundTaskChange change;
    if (granted) {
        change.state = "queued";
        change.permissionApproved = true;
    } else {
        change.state = "failed";
        change.reason = "permission_denied";
        change.neverLaunched = true;
    }
    if (apply(id, change))
        pump();
}

void ManagedTaskRuntime::pump() {
    if (pumping_)
        return;
    QScopedValueRollback guard(pumping_, true);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& id : live_.keys()) {
        auto live = live_.value(id);
        if (live && !live->pendingExecutorEvent.isEmpty()) {
            const auto pending = live->pendingExecutorEvent;
            observeProcess(id, pending);
            live = live_.value(id);
        }
        if (!live || live->record.value("state") == "needs_attention")
            continue;
        if (now >= live->record.value("created_at").toInteger() +
                       live->record.value("timeout_ms").toInteger())
            stopTask(id, "timed_out");
    }
    for (int admitted = 0; admitted < 16; ++admitted) {
        const auto reply = ledger_.claimNext();
        const auto record = reply.value("task").toObject();
        if (!reply.value("ok").toBool() || record.isEmpty())
            break;
        const auto id = record.value("task_id").toString();
        const auto live = live_.value(id);
        if (!live)
            break; // A recovered or external record must be reconciled before execution.
        live->record = record;
        publish(id);
        if (record.value("kind") == "process")
            startProcess(id);
        else if (childStart_ && !reply.value("resume").toBool())
            childStart_(record, live->execution);
    }
}

QJsonObject ManagedTaskRuntime::stop(const QString& id, const BackgroundTaskOwner& owner,
                                     bool localAdministration) {
    const auto authorized = ledger_.get(id, owner, localAdministration);
    if (!authorized.value("ok").toBool())
        return authorized;
    const auto state = authorized.value("task").toObject().value("state").toString();
    if (state == "needs_attention") {
        const auto live = live_.value(id);
        if (live && live->executor)
            live->executor->stop("interrupted");
        else if (live && !live->record.value("executor_stopped").toBool())
            reconcileProcess(id);
        return authorized;
    }
    if (terminal(state) || state == "stopping")
        return authorized;
    stopTask(id, "cancelled");
    return ledger_.get(id, owner, localAdministration);
}

void ManagedTaskRuntime::stopTask(const QString& id, const QString& reason) {
    const auto live = live_.value(id);
    if (!live || live->record.value("state") == "stopping" ||
        live->record.value("state") == "needs_attention")
        return;
    BackgroundTaskChange change;
    change.state = "stopping";
    change.terminalTarget = reason;
    change.reason = reason;
    if (!apply(id, change))
        return;
    live->execution = {};
    if (live->executor) {
        live->executor->stop(reason);
    } else if (live->record.value("kind") == "agent" &&
               !live->record.value("execution_identity").toObject().isEmpty()) {
        if (childStop_)
            childStop_(live->record);
    } else {
        change = {};
        change.state = reason;
        change.reason = reason;
        change.cleanupConfirmed = true;
        change.neverLaunched = true;
        apply(id, change);
    }
}

void ManagedTaskRuntime::cancelOwner(const BackgroundTaskOwner& owner, const QString& reason) {
    QString error;
    ledger_.holdOwner(owner, reason, &error);
    QSet<QString> descendants;
    for (const auto& id : live_.keys()) {
        const auto record = live_.value(id)->record;
        if ((record.value("owner_session_id") == owner.sessionId &&
             record.value("owner_generation").toInteger() == owner.generation) ||
            (record.value("root_owner_session_id") == owner.sessionId &&
             record.value("root_owner_generation").toInteger() == owner.generation))
            descendants.insert(id);
    }
    // Intermediate managed children share a root quota but still have their own
    // ownership subtrees. Closing one must reclaim nested children too.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& id : live_.keys()) {
            if (!descendants.contains(id) &&
                descendants.contains(live_.value(id)->record.value("parent_task_id").toString())) {
                descendants.insert(id);
                changed = true;
            }
        }
    }
    for (const auto& id : descendants)
        stopTask(id, "cancelled");
}

void ManagedTaskRuntime::childChanged(const QString& id, const BackgroundTaskChange& change) {
    apply(id, change);
    pump();
}

QJsonObject ManagedTaskRuntime::reserveChild(const QString& id) {
    const auto live = live_.value(id);
    if (!live)
        return failure("task_not_found", "The managed child intent is no longer active.");
    const auto reply = ledger_.reserveChild(id, live->record.value("revision").toInteger());
    if (reply.value("ok").toBool()) {
        live->record = reply.value("task").toObject();
        publish(id);
    }
    return reply;
}

QJsonObject ManagedTaskRuntime::parkChild(const QString& id) {
    const auto live = live_.value(id);
    if (!live)
        return failure("task_not_found", "The managed child is no longer active.");
    if (live->record.value("parked").toBool())
        return {{"ok", true}, {"task", live->record}};
    const auto reply = ledger_.park(id, live->record.value("revision").toInteger(), true);
    if (reply.value("ok").toBool()) {
        live->record = reply.value("task").toObject();
        publish(id);
    }
    return reply;
}

QJsonObject ManagedTaskRuntime::resumeChild(const QString& id) {
    const auto live = live_.value(id);
    if (!live)
        return failure("task_not_found", "The managed child is no longer active.");
    if (!live->record.value("parked").toBool())
        return {{"ok", true}, {"task", live->record}};
    const auto reply = ledger_.readyToResume(id, live->record.value("revision").toInteger());
    if (reply.value("ok").toBool()) {
        live->record = reply.value("task").toObject();
        pump();
    }
    return reply;
}
} // namespace ketplus::daemon
