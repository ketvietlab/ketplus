#include "storage/BackgroundTaskLedger.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <limits>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
bool terminal(const QString& state) {
    return state == "completed" || state == "failed" || state == "cancelled" ||
           state == "timed_out" || state == "interrupted";
}
QByteArray json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
} // namespace

QJsonObject BackgroundTaskLedger::changeImpl(const QString& id, qint64 expectedRevision,
                                             const BackgroundTaskChange& change,
                                             const QString& userClientId) {
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return failure(code, message);
    };
    auto record = task(id);
    if (record.contains("error_code"))
        return fail(record.value("error_code").toString(),
                    record.value("error_message").toString());
    const QString previous = record.value("state").toString();
    const qint64 revision = record.value("revision").toInteger();
    if (revision != expectedRevision)
        return fail("revision_conflict", "The task revision changed.");
    if (terminal(previous))
        return fail("terminal_immutable", "A terminal task cannot transition again.");
    if (revision == std::numeric_limits<qint64>::max())
        return fail("revision_exhausted", "Task revision exhausted.");
    bool allowed = false;
    const bool native = record.value("containment_mode") == "native_best_effort";
    const bool userAcknowledgement = previous == "needs_attention" && !userClientId.isEmpty() &&
                                     change.reason == "cleanup_acknowledged" &&
                                     record.value("executor_stopped").toBool();
    if (change.state == "queued")
        allowed = previous == "queued" && change.permissionApproved;
    else if (change.state == "starting") {
        const auto registered = record.value("execution_identity").toObject();
        allowed = previous == "starting" && change.handshakeCommitted &&
                  !registered.value("executor_lease_identity").toString().isEmpty() &&
                  registered.value("executor_lease_identity") ==
                      change.executionIdentity.value("executor_lease_identity") &&
                  registered.value("directory") == change.executionIdentity.value("directory") &&
                  !change.executionIdentity.value("supervisor_identity")
                       .toObject()
                       .value("birth")
                       .toString()
                       .isEmpty();
    } else if (change.state == "running") {
        const auto identity = change.executionIdentity;
        const bool processIdentity =
            !identity.value("process_birth_identity").toString().isEmpty() &&
            !identity.value("executor_lease_identity").toString().isEmpty();
        const bool childIdentity = !identity.value("child_session_id").toString().isEmpty() &&
                                   identity.value("child_generation").toInteger() > 0;
        bool linkedChild = false;
        if (record.value("kind") == "agent" && childIdentity) {
            QSqlQuery child(database_);
            child.prepare("SELECT 1 FROM managed_job_children c JOIN sessions s "
                          "ON s.id=c.child_session_id AND s.generation=c.child_generation "
                          "WHERE c.task_id=? AND c.child_session_id=? AND c.child_generation=?");
            child.addBindValue(id);
            child.addBindValue(identity.value("child_session_id").toString());
            child.addBindValue(identity.value("child_generation").toInteger());
            if (!child.exec())
                return fail("storage_error", child.lastError().text());
            linkedChild = child.next();
        }
        allowed = previous == "starting" && change.handshakeCommitted &&
                  (record.value("kind") == "process" ? processIdentity : linkedChild);
    } else if (change.state == "stopping")
        allowed = true;
    else if (change.state == "needs_attention")
        allowed = native && (previous == "starting" || previous == "running" ||
                             previous == "stopping" || previous == "needs_attention");
    else if (change.state == "completed")
        allowed = previous == "running" && change.cleanupConfirmed;
    else if (change.state == "failed") {
        const bool prelaunchFailure = change.reason == "permission_denied" ||
                                      change.reason == "spawn_failed" ||
                                      change.reason == "handshake_failed";
        allowed =
            (previous == "running" && change.cleanupConfirmed) ||
            ((previous == "queued" || previous == "starting") && prelaunchFailure &&
             (change.neverLaunched || change.cleanupConfirmed)) ||
            (previous == "stopping" && record.value("terminal_target").toString() == "failed" &&
             change.cleanupConfirmed);
    } else if (change.state == "cancelled" || change.state == "timed_out")
        allowed = previous == "stopping" && change.cleanupConfirmed;
    else if (change.state == "interrupted")
        allowed = change.cleanupConfirmed || userAcknowledgement;
    if (previous == "needs_attention" && change.state != "needs_attention")
        allowed = change.state == "interrupted" && userAcknowledgement;
    if (!userClientId.isEmpty() && !userAcknowledgement)
        allowed = false;
    if (!allowed)
        return fail("invalid_transition", "The transition lacks its required execution evidence.");
    if (record.value("kind") == "agent" && terminal(change.state)) {
        QSqlQuery descendants(database_);
        descendants.prepare(
            "SELECT 1 FROM managed_jobs WHERE json_extract(record,'$.parent_task_id')=? "
            "AND state NOT IN ('completed','failed','cancelled','timed_out','interrupted') LIMIT "
            "1");
        descendants.addBindValue(id);
        if (!descendants.exec())
            return fail("storage_error", descendants.lastError().text());
        if (descendants.next())
            return fail("execution_live",
                        "A managed agent cannot finish before its descendant jobs.");
        descendants.finish();
        descendants.prepare(
            "SELECT 1 FROM managed_job_children c WHERE c.task_id=? AND "
            "(EXISTS(SELECT 1 FROM managed_job_outbox o WHERE "
            "o.owner_session_id=c.child_session_id "
            "AND o.owner_generation=c.child_generation AND o.status='pending') OR "
            "EXISTS(SELECT 1 FROM managed_job_wakes w WHERE w.owner_session_id=c.child_session_id "
            "AND w.owner_generation=c.child_generation AND w.state IN ('accepted','dispatching'))) "
            "LIMIT 1");
        descendants.addBindValue(id);
        if (!descendants.exec())
            return fail("storage_error", descendants.lastError().text());
        if (descendants.next())
            return fail("execution_live",
                        "A managed child's completion continuations are still pending.");
    }
    if (record.value("containment_mode") == "cgroup_v2" && terminal(change.state) &&
        !change.neverLaunched && (!change.fullScopeVerified || change.cleanupStatus != "verified"))
        return fail("cleanup_pending", "The Linux domain has not been verified empty.");
    if (native && change.cleanupStatus == "verified" && !change.fullScopeVerified)
        return fail("invalid_cleanup_evidence",
                    "Native best-effort cleanup is not kernel verification.");
    if (!change.cleanupStatus.isEmpty() && change.cleanupStatus != "not_required" &&
        change.cleanupStatus != "pending" && change.cleanupStatus != "verified" &&
        change.cleanupStatus != "unverified" && change.cleanupStatus != "best_effort_complete")
        return fail("invalid_cleanup_evidence", "Unknown cleanup assurance level.");
    if ((!native && change.cleanupStatus == "best_effort_complete") ||
        (terminal(change.state) && !userAcknowledgement &&
         (change.cleanupStatus == "unverified" || change.cleanupStatus == "pending")))
        return fail("invalid_cleanup_evidence",
                    "The cleanup assurance cannot support this result.");
    record.insert("state", change.state);
    record.insert("revision", revision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    record.insert("reason", change.reason);
    if (change.executorStopped)
        record.insert("executor_stopped", *change.executorStopped);
    if (change.exitCode)
        record.insert("exit_code", *change.exitCode);
    if (change.outputSize && *change.outputSize >= 0)
        record.insert("output_size", *change.outputSize);
    if (change.outputTruncated)
        record.insert("output_truncated", *change.outputTruncated);
    if (!change.errorCode.isEmpty())
        record.insert("error", change.errorCode);
    if (change.state == "running")
        record.insert("executor_stopped", false);
    if (change.state == "stopping")
        record.insert("cleanup_status", "pending");
    if (!change.cleanupStatus.isEmpty())
        record.insert("cleanup_status", change.cleanupStatus);
    if (change.state == "needs_attention") {
        record.insert("cleanup_status", "unverified");
        record.insert("reason", "cleanup_unverified");
        if (record.value("executor_stopped").toBool()) {
            record.insert("slot_held", false);
            record.insert("parked", false);
        }
        record.insert("manual_guidance", "Check the recorded executor and descendants, then "
                                         "acknowledge cleanup in the task UI.");
    }
    if (change.permissionApproved)
        record.insert("permission_approved", true);
    if (change.handshakeCommitted)
        record.insert("handshake_committed", true);
    record.insert("cleanup_pending", !terminal(change.state) && change.cleanupPending);
    if (terminal(change.state)) {
        record.insert("slot_held", false);
        record.insert("parked", false);
        if (!userAcknowledgement) {
            record.insert("executor_stopped", true);
            record.insert("cleanup_status", change.neverLaunched              ? "not_required"
                                            : !change.cleanupStatus.isEmpty() ? change.cleanupStatus
                                            : native ? "best_effort_complete"
                                                     : "verified");
        }
    }
    if (!change.terminalTarget.isEmpty())
        record.insert("terminal_target", change.terminalTarget);
    if (!change.executionIdentity.isEmpty())
        record.insert("execution_identity", change.executionIdentity);
    QSqlQuery query(database_);
    if (userAcknowledgement) {
        query.prepare(
            "INSERT INTO managed_job_cleanup_audit(task_id,revision,source,user_client_id,"
            "recorded_at) VALUES(?,?,'user',?,?)");
        query.addBindValue(id);
        query.addBindValue(revision + 1);
        query.addBindValue(userClientId);
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!query.exec())
            return fail("storage_error", query.lastError().text());
    }
    if (terminal(change.state)) {
        const QString eventId = id + ":" + QString::number(revision + 1);
        record.insert("completion_event_id", eventId);
        record.insert("ended_at", record.value("updated_at"));
        QString suppressed = change.suppressReason;
        if (change.reason == "permission_denied")
            suppressed = "permission_denied";
        if (change.state == "cancelled" && suppressed.isEmpty())
            suppressed = "cancelled";
        const QJsonObject payload{{"task_id", id},
                                  {"state", change.state},
                                  {"summary", change.summary},
                                  {"output_reference", id}};
        query.prepare("INSERT INTO managed_job_outbox(event_id,task_id,owner_session_id,"
                      "owner_generation,payload,status,reason) VALUES(?,?,?,?,?,?,?)");
        query.addBindValue(eventId);
        query.addBindValue(id);
        query.addBindValue(record.value("owner_session_id").toString());
        query.addBindValue(record.value("owner_generation").toInteger());
        query.addBindValue(json(payload));
        query.addBindValue(suppressed.isEmpty() ? "pending" : "suppressed");
        query.addBindValue(suppressed.isEmpty() ? QVariant{} : QVariant(suppressed));
        if (!query.exec())
            return fail("storage_error", query.lastError().text());
    }
    query.prepare("UPDATE managed_jobs SET record=?,state=?,"
                  "holds_slot=CASE WHEN ? THEN 0 ELSE holds_slot END,"
                  "parked=CASE WHEN ? THEN 0 ELSE parked END WHERE task_id=?");
    query.addBindValue(json(record));
    query.addBindValue(change.state);
    const bool releaseSlot = terminal(change.state) || (change.state == "needs_attention" &&
                                                        record.value("executor_stopped").toBool());
    query.addBindValue(releaseSlot);
    query.addBindValue(releaseSlot);
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", task(id)}};
}

} // namespace ketplus::daemon
