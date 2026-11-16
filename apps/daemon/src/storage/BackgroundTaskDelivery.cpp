#include "storage/BackgroundTaskLedger.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
bool currentOwner(QSqlDatabase database, const BackgroundTaskOwner& owner) {
    QSqlQuery query(database);
    query.prepare("SELECT 1 FROM sessions WHERE id=? AND generation=?");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    return query.exec() && query.next();
}
} // namespace

QJsonObject BackgroundTaskLedger::acceptCompletion(const QString& eventId,
                                                   const BackgroundTaskOwner& owner) {
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return failure(code, message);
    };
    if (!currentOwner(database_, owner))
        return fail("stale_owner", "The owner generation is no longer current.");
    QSqlQuery query(database_);
    query.prepare("SELECT reason FROM managed_job_owner_fences WHERE owner_session_id=? "
                  "AND owner_generation=?");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (query.next())
        return fail("owner_held", query.value(0).toString());
    query.finish();
    query.prepare("SELECT payload,status,owner_session_id,owner_generation FROM managed_job_outbox "
                  "WHERE event_id=?");
    query.addBindValue(eventId);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!query.next() || query.value(2).toString() != owner.sessionId ||
        query.value(3).toLongLong() != owner.generation)
        return fail("stale_owner", "The completion does not belong to this owner generation.");
    const QByteArray payload = query.value(0).toByteArray();
    const QString status = query.value(1).toString();
    query.finish();
    if (status == "suppressed")
        return fail("completion_suppressed", "This completion cannot wake its owner.");
    query.prepare(
        "INSERT OR IGNORE INTO managed_job_wakes(event_id,owner_session_id,owner_generation,"
        "turn_record,state) VALUES(?,?,?,?,'accepted')");
    query.addBindValue(eventId);
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    query.addBindValue(payload);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    const bool duplicate = query.numRowsAffected() == 0;
    query.prepare("UPDATE managed_job_outbox SET status='delivered' WHERE event_id=?");
    query.addBindValue(eventId);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"duplicate", duplicate}, {"event_id", eventId}};
}

QJsonObject BackgroundTaskLedger::claimWake(const QString& eventId,
                                            const BackgroundTaskOwner& owner) {
    QSqlQuery query(database_);
    query.prepare("UPDATE managed_job_wakes SET state='dispatching' WHERE event_id=? "
                  "AND owner_session_id=? AND owner_generation=? AND state='accepted' "
                  "AND EXISTS(SELECT 1 FROM sessions WHERE id=owner_session_id "
                  "AND generation=owner_generation) "
                  "AND NOT EXISTS(SELECT 1 FROM managed_job_owner_fences f "
                  "WHERE f.owner_session_id=managed_job_wakes.owner_session_id "
                  "AND f.owner_generation=managed_job_wakes.owner_generation)");
    query.addBindValue(eventId);
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    if (query.numRowsAffected() != 1)
        return failure("wake_not_dispatchable",
                       "This wake was claimed or belongs to another owner.");
    return {{"ok", true}, {"event_id", eventId}};
}

bool BackgroundTaskLedger::holdOwner(const BackgroundTaskOwner& owner, const QString& reason,
                                     QString* error) {
    QSqlQuery query(database_);
    if (reason.isEmpty())
        query.prepare("DELETE FROM managed_job_owner_fences WHERE owner_session_id=? "
                      "AND owner_generation=?");
    else
        query.prepare(
            "INSERT INTO managed_job_owner_fences(owner_session_id,owner_generation,reason) "
            "VALUES(?,?,?) ON CONFLICT(owner_session_id,owner_generation) "
            "DO UPDATE SET reason=excluded.reason");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    if (!reason.isEmpty())
        query.addBindValue(reason);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    return true;
}

} // namespace ketplus::daemon
