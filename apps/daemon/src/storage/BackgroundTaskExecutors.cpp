#include "storage/BackgroundTaskLedger.h"
#include "storage/RemoteJobRows.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <limits>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
} // namespace

QJsonObject BackgroundTaskLedger::retry(const BackgroundTaskIntent& intent) const {
    QSqlQuery query(database_);
    query.prepare(
        "SELECT task_id,spec_fingerprint,json_extract(record,'$.execution.request_defaults'),0 "
        "FROM managed_jobs WHERE owner_session_id=? AND owner_generation=? AND client_request_id=? "
        "UNION ALL SELECT task_id,spec_hash,defaults_snapshot,1 FROM managed_job_tombstones "
        "WHERE owner_session_id=? AND owner_generation=? AND client_request_id=? LIMIT 1");
    query.addBindValue(intent.owner.sessionId);
    query.addBindValue(intent.owner.generation);
    query.addBindValue(intent.clientRequestId);
    query.addBindValue(intent.owner.sessionId);
    query.addBindValue(intent.owner.generation);
    query.addBindValue(intent.clientRequestId);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    if (!query.next())
        return {{"ok", true}, {"task", QJsonValue::Null}};
    const auto id = query.value(0).toString();
    const auto fingerprint = query.value(1).toByteArray();
    const auto defaults = QJsonDocument::fromJson(query.value(2).toByteArray()).object();
    const bool expired = query.value(3).toBool();
    query.finish();
    QSqlQuery current(database_);
    current.prepare("SELECT 1 FROM sessions WHERE id=? AND generation=?");
    current.addBindValue(intent.owner.sessionId);
    current.addBindValue(intent.owner.generation);
    if (!current.exec())
        return failure("storage_error", current.lastError().text());
    if (!current.next())
        return failure("stale_owner", "The owner generation is no longer current.");
    const auto requested = intent.requestPayload.isEmpty()
                               ? intent.specFingerprint
                               : backgroundTaskFingerprint(intent.requestPayload, defaults);
    if (fingerprint != remote_job_rows::fingerprint(requested, intent.runsOn, intent.timeoutMs))
        return failure("idempotency_conflict",
                       "This request key was accepted with a different spec.");
    if (expired)
        return {{"ok", false}, {"error_code", "task_expired"}, {"task_id", id}, {"expired", true}};
    auto reply = get(id, intent.owner);
    if (!reply.value("ok").toBool())
        return reply;
    reply.insert("reused", true);
    return reply;
}

QJsonObject BackgroundTaskLedger::nonterminal() const {
    QSqlQuery query(database_);
    if (!query.exec(
            "SELECT task_id FROM managed_jobs WHERE state NOT IN "
            "('completed','failed','cancelled','timed_out','interrupted') AND NOT EXISTS(SELECT 1 "
            "FROM remote_jobs x WHERE x.task_id=managed_jobs.task_id AND x.revision>0) "
            "ORDER BY sequence"))
        return failure("storage_error", query.lastError().text());
    QStringList ids;
    while (query.next())
        ids.append(query.value(0).toString());
    query.finish();
    QJsonArray records;
    for (const auto& id : ids) {
        const auto record = task(id);
        if (record.contains("error_code"))
            return record;
        records.append(record);
    }
    return {{"ok", true}, {"tasks", records}};
}

QJsonObject BackgroundTaskLedger::registerExecutor(const QString& id, qint64 expectedRevision,
                                                   const QJsonObject& identity) {
    if (identity.value("executor_lease_identity").toString().isEmpty() ||
        identity.value("directory").toString().isEmpty())
        return failure("invalid_execution_identity",
                       "A durable executor address and lease are required.");
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return failure(code, message);
    };
    if (const auto refused = remote_job_rows::localExecutionRefusal(database_, id);
        !refused.isEmpty())
        return fail(refused, "A remote job never runs locally.");
    auto record = task(id);
    if (record.contains("error_code"))
        return fail(record.value("error_code").toString(),
                    record.value("error_message").toString());
    if (record.value("revision").toInteger() != expectedRevision)
        return fail("revision_conflict", "The task revision changed.");
    if (record.value("state") != "starting" || record.value("kind") != "process" ||
        !record.value("permission_approved").toBool() ||
        !record.value("execution_identity").toObject().isEmpty() ||
        expectedRevision == std::numeric_limits<qint64>::max())
        return fail("invalid_transition",
                    "Only an approved, unbound process can register its executor.");
    record.insert("execution_identity", identity);
    record.insert("revision", expectedRevision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    QSqlQuery query(database_);
    query.prepare("UPDATE managed_jobs SET record=? WHERE task_id=?");
    query.addBindValue(QJsonDocument(record).toJson(QJsonDocument::Compact));
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", record}};
}
} // namespace ketplus::daemon
