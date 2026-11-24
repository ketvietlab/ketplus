#include "runner/entry/RemoteGuidance.h"
#include "storage/BackgroundTaskLedger.h"
#include "storage/RemoteJobRows.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

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

QJsonObject BackgroundTaskLedger::applyTransition(const QString& id, QJsonObject record,
                                                  qint64 revision,
                                                  const BackgroundTaskChange& change,
                                                  const BackgroundTaskEffects& effects) {
    const QString previous = record.value("state").toString();
    std::optional<bool> stopped = effects.executorStopped;
    const auto stop = [&](const bool value) {
        if (!stopped)
            return;
        stopped = value;
        record.insert("executor_stopped", value);
    };
    record.insert("state", change.state);
    record.insert("revision", revision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    record.insert("reason", change.reason);
    if (change.executorStopped)
        stop(*change.executorStopped);
    if (change.exitCode)
        record.insert("exit_code", *change.exitCode);
    if (change.outputSize && *change.outputSize >= 0)
        record.insert("output_size", *change.outputSize);
    if (change.outputTruncated)
        record.insert("output_truncated", *change.outputTruncated);
    if (!change.errorCode.isEmpty())
        record.insert("error", change.errorCode);
    if (change.state == "running")
        stop(false);
    if (change.state == "stopping")
        record.insert("cleanup_status", "pending");
    if (!change.cleanupStatus.isEmpty())
        record.insert("cleanup_status", change.cleanupStatus);
    if (change.state == "needs_attention") {
        record.insert("cleanup_status", "unverified");
        record.insert("reason", "cleanup_unverified");
        if (stopped.value_or(false)) {
            record.insert("slot_held", false);
            record.insert("parked", false);
        }
        record.insert("manual_guidance", "Check the recorded executor and descendants, then "
                                         "acknowledge cleanup in the task UI.");
        if (remote_job_rows::phaseOf(database_, id).value_or(remote_job_rows::Phase::Local) !=
            remote_job_rows::Phase::Local)
            record.insert("manual_guidance", runner::entry::remoteManualGuidance());
    }
    if (change.permissionApproved)
        record.insert("permission_approved", true);
    if (change.handshakeCommitted)
        record.insert("handshake_committed", true);
    record.insert("cleanup_pending", !terminal(change.state) && change.cleanupPending);
    if (terminal(change.state)) {
        record.insert("slot_held", false);
        record.insert("parked", false);
        if (!effects.userAcknowledgement) {
            stop(true);
            record.insert("cleanup_status", change.neverLaunched              ? "not_required"
                                            : !change.cleanupStatus.isEmpty() ? change.cleanupStatus
                                            : effects.native ? "best_effort_complete"
                                                             : "verified");
        }
    }
    if (!change.terminalTarget.isEmpty())
        record.insert("terminal_target", change.terminalTarget);
    if (!change.executionIdentity.isEmpty())
        record.insert("execution_identity", change.executionIdentity);
    QSqlQuery query(database_);
    if (effects.userAcknowledgement) {
        query.prepare(
            "INSERT INTO managed_job_cleanup_audit(task_id,revision,source,user_client_id,"
            "recorded_at) VALUES(?,?,?,?,?)");
        query.addBindValue(id);
        query.addBindValue(revision + 1);
        query.addBindValue(effects.auditSource);
        query.addBindValue(effects.auditClientId);
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!query.exec())
            return failure("storage_error", query.lastError().text());
    }
    if (change.state == "needs_attention") {
        const auto noticed = noticeAttention(id, record, previous);
        if (!noticed.value("ok").toBool())
            return noticed;
    }
    if (terminal(change.state)) {
        const auto superseded = supersedeNotices(id);
        if (!superseded.value("ok").toBool())
            return superseded;
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
            return failure("storage_error", query.lastError().text());
    }
    query.prepare("UPDATE managed_jobs SET record=?,state=?,"
                  "holds_slot=CASE WHEN ? THEN 0 ELSE holds_slot END,"
                  "parked=CASE WHEN ? THEN 0 ELSE parked END WHERE task_id=?");
    query.addBindValue(json(record));
    query.addBindValue(change.state);
    const bool releaseSlot =
        terminal(change.state) || (change.state == "needs_attention" && stopped.value_or(false));
    query.addBindValue(releaseSlot);
    query.addBindValue(releaseSlot);
    query.addBindValue(id);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}

} // namespace ketplus::daemon
