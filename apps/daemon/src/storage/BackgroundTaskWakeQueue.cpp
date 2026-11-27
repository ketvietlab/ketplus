#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

QJsonObject BackgroundTaskLedger::linkedTask(const QString& child, qint64 generation) const {
    QSqlQuery query(database_);
    query.prepare(
        "SELECT task_id FROM managed_job_children WHERE child_session_id=? AND child_generation=?");
    query.addBindValue(child);
    query.addBindValue(generation);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    if (!query.next())
        return {{"ok", true}, {"task", QJsonValue::Null}};
    const auto id = query.value(0).toString();
    query.finish();
    const auto record = task(id);
    if (record.value("error_code") == "task_not_found")
        return {{"ok", false}, {"error_code", "task_expired"}, {"task_id", id}, {"expired", true}};
    if (record.contains("error_code"))
        return record;
    return {{"ok", true}, {"task", record}};
}

QJsonObject BackgroundTaskLedger::deliveryPage(bool wakes, qint64 after) const {
    QSqlQuery query(database_);
    query.prepare(
        wakes ? "SELECT rowid,event_id,owner_session_id,owner_generation,turn_record "
                "FROM managed_job_wakes WHERE state='accepted' AND rowid>? ORDER BY rowid LIMIT 100"
              : "SELECT rowid,event_id,owner_session_id,owner_generation,payload "
                "FROM managed_job_outbox WHERE status='pending' AND rowid>? ORDER BY rowid LIMIT "
                "100");
    query.addBindValue(after);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    QJsonArray events;
    qint64 next = 0;
    while (query.next()) {
        next = query.value(0).toLongLong();
        events.append(QJsonObject{
            {"event_id", query.value(1).toString()},
            {"owner_session_id", query.value(2).toString()},
            {"owner_generation", query.value(3).toLongLong()},
            {"turn_record", QJsonDocument::fromJson(query.value(4).toByteArray()).object()}});
    }
    return {{"ok", true}, {"events", events}, {"next", events.size() == 100 ? next : 0}};
}

QJsonObject BackgroundTaskLedger::suppressCompletion(const QString& eventId,
                                                     const QString& reason) {
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    QSqlQuery query(database_);
    for (const auto* table : {"managed_job_outbox", "managed_job_notices"}) {
        query.prepare(QStringLiteral("UPDATE %1 SET status='suppressed',reason=? WHERE event_id=?")
                          .arg(QLatin1String(table)));
        query.addBindValue(reason);
        query.addBindValue(eventId);
        if (!query.exec()) {
            database_.rollback();
            return failure("storage_error", query.lastError().text());
        }
    }
    query.prepare("UPDATE managed_job_wakes SET state='suppressed' WHERE event_id=?");
    query.addBindValue(eventId);
    if (!query.exec() || !database_.commit()) {
        database_.rollback();
        return failure("storage_error", query.lastError().text());
    }
    return {{"ok", true}};
}

QJsonObject BackgroundTaskLedger::finishWake(const QString& eventId, const QString& state) {
    if (state != "finished" && state != "interrupted")
        return failure("invalid_request", "Unknown completed wake state.");
    QSqlQuery query(database_);
    query.prepare("UPDATE managed_job_wakes SET state=? WHERE event_id=? AND state='dispatching'");
    query.addBindValue(state);
    query.addBindValue(eventId);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}

QJsonObject BackgroundTaskLedger::recoverDispatchingWakes() {
    QSqlQuery query(database_);
    if (!query.exec("UPDATE managed_job_wakes SET state='interrupted' WHERE state='dispatching'"))
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}
QJsonObject BackgroundTaskLedger::deferWake(const QString& eventId) {
    // Only the live dispatcher calls this before invoking any provider method.
    // Recovery never reclaims an uncertain dispatching inference through this path.
    QSqlQuery query(database_);
    query.prepare(
        "UPDATE managed_job_wakes SET state='accepted' WHERE event_id=? AND state='dispatching'");
    query.addBindValue(eventId);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    return {{"ok", true}};
}

bool BackgroundTaskLedger::wakeDispatchAllowed(const QString& eventId,
                                               const BackgroundTaskOwner& owner) const {
    QSqlQuery query(database_);
    query.prepare("SELECT 1 FROM managed_job_wakes w JOIN sessions s ON s.id=w.owner_session_id "
                  "AND s.generation=w.owner_generation WHERE w.event_id=? AND w.owner_session_id=? "
                  "AND w.owner_generation=? AND w.state='dispatching' AND NOT EXISTS(SELECT 1 "
                  "FROM managed_job_owner_fences f WHERE f.owner_session_id=w.owner_session_id "
                  "AND f.owner_generation=w.owner_generation)");
    query.addBindValue(eventId);
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    return query.exec() && query.next();
}
} // namespace ketplus::daemon
