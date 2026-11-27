#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {
QJsonObject BackgroundTaskLedger::prune(qint64 nowMs) {
    const auto failure = [](const QString& message) {
        return QJsonObject{
            {"ok", false}, {"error_code", "storage_error"}, {"error_message", message}};
    };
    if (!database_.transaction())
        return failure(database_.lastError().text());
    const auto fail = [&](const QString& message) {
        database_.rollback();
        return failure(message);
    };
    QSqlQuery query(database_);
    query.prepare(
        "SELECT j.task_id,j.owner_session_id,j.owner_generation,j.client_request_id,"
        "j.spec_fingerprint,json_extract(j.record,'$.execution.request_defaults') FROM "
        "managed_jobs j "
        "WHERE j.state IN ('completed','failed','cancelled','timed_out','interrupted') "
        "AND json_extract(j.record,'$.ended_at')<=? "
        "AND NOT EXISTS(SELECT 1 FROM managed_job_outbox o WHERE o.task_id=j.task_id AND "
        "o.status='pending') "
        "AND NOT EXISTS(SELECT 1 FROM managed_job_wakes w JOIN managed_job_outbox o ON "
        "o.event_id=w.event_id "
        "WHERE o.task_id=j.task_id AND w.state IN ('accepted','dispatching')) "
        "AND NOT EXISTS(SELECT 1 FROM managed_jobs p WHERE "
        "p.task_id=json_extract(j.record,'$.parent_task_id') "
        "AND p.state NOT IN ('completed','failed','cancelled','timed_out','interrupted')) "
        "AND NOT EXISTS(SELECT 1 FROM managed_job_children c JOIN managed_jobs d "
        "ON d.owner_session_id=c.child_session_id AND d.owner_generation=c.child_generation "
        "WHERE c.task_id=j.task_id AND d.state NOT IN "
        "('completed','failed','cancelled','timed_out','interrupted')) "
        "ORDER BY j.sequence LIMIT 100");
    query.addBindValue(nowMs - 7LL * 24 * 60 * 60 * 1000);
    if (!query.exec())
        return fail(query.lastError().text());
    struct Expired {
        QString id, owner, key;
        qint64 generation;
        QByteArray hash, defaults;
    };
    QList<Expired> expired;
    while (query.next())
        expired.append({query.value(0).toString(), query.value(1).toString(),
                        query.value(3).toString(), query.value(2).toLongLong(),
                        query.value(4).toByteArray(),
                        query.value(5).isNull() ? QByteArray("{}") : query.value(5).toByteArray()});
    query.finish();
    for (const auto& item : expired) {
        query.prepare("INSERT INTO managed_job_tombstones(owner_session_id,owner_generation,"
                      "client_request_id,spec_hash,task_id,expired_at,defaults_snapshot) "
                      "VALUES(?,?,?,?,?,?,?)");
        query.addBindValue(item.owner);
        query.addBindValue(item.generation);
        query.addBindValue(item.key);
        query.addBindValue(item.hash);
        query.addBindValue(item.id);
        query.addBindValue(nowMs);
        query.addBindValue(item.defaults);
        if (!query.exec())
            return fail(query.lastError().text());
        query.prepare("DELETE FROM managed_job_wakes WHERE event_id IN (SELECT event_id FROM "
                      "managed_job_outbox WHERE task_id=? UNION SELECT event_id FROM "
                      "managed_job_notices WHERE task_id=?)");
        query.addBindValue(item.id);
        query.addBindValue(item.id);
        if (!query.exec())
            return fail(query.lastError().text());
        // The compact child link remains an ownership/dedup fence after the
        // parent's metadata expires. A worker cannot become a new root by aging.
        for (const auto& table : {"managed_job_outbox", "managed_job_notices", "managed_jobs"}) {
            query.prepare(QStringLiteral("DELETE FROM %1 WHERE task_id=?").arg(table));
            query.addBindValue(item.id);
            if (!query.exec())
                return fail(query.lastError().text());
        }
    }
    if (!database_.commit())
        return fail(database_.lastError().text());
    // A crash between metadata commit and deleting the spool leaves this durable
    // cleanup queue; it cannot resurrect an expired execution request.
    if (!query.exec("SELECT task_id FROM managed_job_tombstones WHERE output_removed=0 LIMIT 100"))
        return failure(query.lastError().text());
    QJsonArray cleanup;
    while (query.next())
        cleanup.append(query.value(0).toString());
    return {{"ok", true}, {"output_cleanup", cleanup}, {"pruned", expired.size()}};
}

bool BackgroundTaskLedger::outputRemoved(const QString& id) {
    QSqlQuery query(database_);
    query.prepare("UPDATE managed_job_tombstones SET output_removed=1 WHERE task_id=?");
    query.addBindValue(id);
    return query.exec();
}
} // namespace ketplus::daemon
