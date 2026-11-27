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
    if (change.permissionApproved)
        record.insert("permission_approved", true);
    if (change.handshakeCommitted)
        record.insert("handshake_committed", true);
    record.insert("cleanup_pending", !terminal(change.state) && change.cleanupPending);
    if (terminal(change.state)) {
        record.insert("slot_held", false);
        record.insert("parked", false);
        stop(true);
        record.insert("cleanup_status", change.neverLaunched              ? "not_required"
                                        : !change.cleanupStatus.isEmpty() ? change.cleanupStatus
                                        : effects.native                  ? "best_effort_complete"
                                                                          : "verified");
    }
    // The evidence kind lives only as long as the verified status it supports.
    if (change.groupVerified && record.value("cleanup_status") == "verified")
        record.insert("cleanup_verification", "process_group_empty");
    else if (record.value("cleanup_status") != "verified")
        record.remove("cleanup_verification");
    if (!change.terminalTarget.isEmpty())
        record.insert("terminal_target", change.terminalTarget);
    if (!change.executionIdentity.isEmpty())
        record.insert("execution_identity", change.executionIdentity);
    QSqlQuery query(database_);
    if (!change.cleanupDiagnostic.isEmpty()) {
        // Internal diagnostics only: never part of the task record clients and agents read.
        query.prepare("INSERT OR REPLACE INTO managed_job_diagnostics(task_id,revision,issue,"
                      "recorded_at) VALUES(?,?,?,?)");
        query.addBindValue(id);
        query.addBindValue(revision + 1);
        query.addBindValue(change.cleanupDiagnostic);
        query.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!query.exec())
            return failure("storage_error", query.lastError().text());
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
    const bool releaseSlot = terminal(change.state);
    query.addBindValue(releaseSlot);
    query.addBindValue(releaseSlot);
    query.addBindValue(id);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}

} // namespace ketplus::daemon
