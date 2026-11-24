#include "storage/BackgroundTaskLedger.h"
#include "runner/jobs/RemoteJobLabels.h"
#include "storage/RemoteJobRows.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <utility>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
QByteArray json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
bool sameOwner(const QJsonObject& task, const BackgroundTaskOwner& owner) {
    return task.value("owner_session_id").toString() == owner.sessionId &&
           task.value("owner_generation").toInteger() == owner.generation;
}
bool currentOwner(QSqlDatabase database, const BackgroundTaskOwner& owner) {
    QSqlQuery query(database);
    query.prepare("SELECT 1 FROM sessions WHERE id=? AND generation=?");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    return query.exec() && query.next();
}
} // namespace

BackgroundTaskLedger::BackgroundTaskLedger(QSqlDatabase database)
    : database_(std::move(database)) {}

bool BackgroundTaskLedger::initialize(QString* error) {
    QSqlQuery query(database_);
    for (const auto& statement :
         {"CREATE TABLE IF NOT EXISTS managed_job_tombstones(owner_session_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL,client_request_id TEXT NOT NULL,spec_hash BLOB NOT "
          "NULL,"
          "task_id TEXT NOT NULL UNIQUE,expired_at INTEGER NOT NULL,defaults_snapshot BLOB NOT "
          "NULL,"
          "output_removed INTEGER NOT NULL DEFAULT 0,PRIMARY "
          "KEY(owner_session_id,owner_generation,client_request_id))",
          "CREATE TABLE IF NOT EXISTS managed_jobs (sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
          "task_id TEXT NOT NULL UNIQUE, "
          "owner_session_id TEXT NOT NULL,owner_generation INTEGER NOT NULL,"
          "root_session_id TEXT NOT NULL,root_generation INTEGER NOT NULL,"
          "state TEXT NOT NULL,holds_slot INTEGER NOT NULL DEFAULT 0,"
          "parked INTEGER NOT NULL DEFAULT 0,resume_ready INTEGER NOT NULL DEFAULT 0,"
          "client_request_id TEXT NOT NULL,spec_fingerprint BLOB NOT NULL,"
          "record BLOB NOT NULL,UNIQUE(owner_session_id,owner_generation,client_request_id))",
          "CREATE INDEX IF NOT EXISTS managed_jobs_slots ON "
          "managed_jobs(holds_slot,root_session_id,root_generation)",
          "CREATE INDEX IF NOT EXISTS managed_jobs_descendants ON "
          "managed_jobs(json_extract(record,'$.parent_task_id'),state)",
          "CREATE INDEX IF NOT EXISTS managed_jobs_nonterminal ON managed_jobs(sequence) WHERE "
          "state NOT IN ('completed','failed','cancelled','timed_out','interrupted')",
          "CREATE TABLE IF NOT EXISTS managed_job_children(task_id TEXT PRIMARY KEY,"
          "child_session_id TEXT NOT NULL UNIQUE,child_generation INTEGER NOT NULL)",
          "CREATE TABLE IF NOT EXISTS managed_job_outbox (event_id TEXT PRIMARY KEY,"
          "task_id TEXT NOT NULL UNIQUE,owner_session_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL,payload BLOB NOT NULL,status TEXT NOT NULL,reason "
          "TEXT)",
          "CREATE TABLE IF NOT EXISTS managed_job_wakes (event_id TEXT PRIMARY KEY,"
          "owner_session_id TEXT NOT NULL,owner_generation INTEGER NOT NULL,"
          "turn_record BLOB NOT NULL,state TEXT NOT NULL)",
          // Owner notices of a nonterminal job (needs_attention, still queued). The event id
          // makes each one idempotent; the completion outbox keeps one event per task.
          "CREATE TABLE IF NOT EXISTS managed_job_notices (event_id TEXT PRIMARY KEY,"
          "task_id TEXT NOT NULL,kind TEXT NOT NULL,owner_session_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL,payload BLOB NOT NULL,status TEXT NOT NULL,"
          "reason TEXT)",
          "CREATE INDEX IF NOT EXISTS managed_job_notices_task ON managed_job_notices(task_id)"}) {
        if (!query.exec(QLatin1String(statement))) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS managed_job_cleanup_audit(task_id TEXT NOT NULL,"
                    "revision INTEGER NOT NULL,source TEXT NOT NULL,user_client_id TEXT NOT NULL,"
                    "recorded_at INTEGER NOT NULL,PRIMARY KEY(task_id,revision))")) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    if (!query.exec("CREATE TABLE IF NOT EXISTS managed_job_owner_fences ("
                    "owner_session_id TEXT NOT NULL,owner_generation INTEGER NOT NULL,"
                    "reason TEXT NOT NULL,PRIMARY KEY(owner_session_id,owner_generation))")) {
        if (error)
            *error = query.lastError().text();
        return false;
    }
    for (const auto& statement :
         {"CREATE TABLE IF NOT EXISTS managed_job_clock(singleton INTEGER PRIMARY KEY "
          "CHECK(singleton=1),revision INTEGER NOT NULL CHECK(typeof(revision)='integer' "
          "AND revision>0))",
          "INSERT OR IGNORE INTO managed_job_clock VALUES(1,1)",
          "CREATE TRIGGER IF NOT EXISTS managed_job_insert_clock AFTER INSERT ON managed_jobs "
          "BEGIN UPDATE managed_job_clock SET revision=revision+1; END",
          "CREATE TRIGGER IF NOT EXISTS managed_job_update_clock AFTER UPDATE ON managed_jobs "
          "BEGIN UPDATE managed_job_clock SET revision=revision+1; END",
          "CREATE TRIGGER IF NOT EXISTS managed_job_delete_clock AFTER DELETE ON managed_jobs "
          "BEGIN UPDATE managed_job_clock SET revision=revision+1; END",
          "CREATE TRIGGER IF NOT EXISTS managed_job_delivery_clock AFTER UPDATE ON "
          "managed_job_outbox BEGIN UPDATE managed_job_clock SET revision=revision+1; END"}) {
        if (!query.exec(QLatin1String(statement))) {
            if (error)
                *error = query.lastError().text();
            return false;
        }
    }
    if (!remote_job_rows::initialize(database_, error))
        return false;
    return true;
}

QJsonObject BackgroundTaskLedger::task(const QString& taskId) const {
    QSqlQuery query(database_);
    query.prepare("SELECT j.record,o.event_id,o.status,o.reason FROM managed_jobs j "
                  "LEFT JOIN managed_job_outbox o ON o.task_id=j.task_id WHERE j.task_id=?");
    query.addBindValue(taskId);
    if (!query.exec())
        return failure("storage_error", query.lastError().text());
    if (!query.next())
        return failure("task_not_found", "This task does not exist.");
    auto record = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    if (record.value("schema_version").toInt() != 1 ||
        record.value("task_id").toString() != taskId || record.value("revision").toInteger() <= 0)
        return failure("storage_error", "The task record is corrupt or has an unsupported schema.");
    if (!query.value(1).isNull())
        record.insert("completion",
                      QJsonObject{{"event_id", query.value(1).toString()},
                                  {"status", query.value(2).toString()},
                                  {"reason", query.value(3).isNull()
                                                 ? QJsonValue(QJsonValue::Null)
                                                 : QJsonValue(query.value(3).toString())}});
    return record;
}

QJsonObject BackgroundTaskLedger::get(const QString& taskId, const BackgroundTaskOwner& owner,
                                      bool localAdministration) const {
    if (!localAdministration && !currentOwner(database_, owner))
        return failure("stale_owner", "The owner generation is no longer current.");
    auto record = task(taskId);
    if (record.contains("error_code"))
        return record;
    if (!sameOwner(record, owner))
        return failure("task_not_found", "This task does not belong to this owner generation.");
    return {{"ok", true}, {"task", record}};
}

QJsonObject BackgroundTaskLedger::create(const BackgroundTaskIntent& intent) {
    if (intent.runsOn && !runner::jobs::isValidRunsOn(*intent.runsOn))
        return failure("invalid_labels", "The runner labels are not valid.");
    if (intent.owner.sessionId.isEmpty() || intent.owner.generation <= 0 ||
        intent.clientRequestId.isEmpty() || intent.specFingerprint.isEmpty() ||
        (intent.kind != "process" && intent.kind != "agent") || intent.timeoutMs <= 0)
        return failure("invalid_request", "Incomplete accepted task intent.");
    const auto existing = retry(intent);
    if (!existing.value("ok").toBool() || !existing.value("task").isNull())
        return existing;
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return failure(code, message);
    };
    if (!currentOwner(database_, intent.owner))
        return fail("stale_owner", "The owner generation is no longer current.");
    QSqlQuery query(database_);
    query.prepare("SELECT task_id,spec_fingerprint FROM managed_jobs WHERE "
                  "owner_session_id=? AND owner_generation=? AND client_request_id=?");
    query.addBindValue(intent.owner.sessionId);
    query.addBindValue(intent.owner.generation);
    query.addBindValue(intent.clientRequestId);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (query.next()) {
        const QString id = query.value(0).toString();
        const bool matches =
            query.value(1).toByteArray() ==
            remote_job_rows::fingerprint(intent.specFingerprint, intent.runsOn, intent.timeoutMs);
        query.finish();
        database_.rollback();
        if (!matches)
            return failure("idempotency_conflict", "This request key has a different task spec.");
        return {{"ok", true}, {"task", task(id)}, {"reused", true}};
    }
    query.finish();
    if (!query.exec(
            "SELECT sum(state='queued'),sum(state NOT IN "
            "('completed','failed','cancelled','timed_out','interrupted')) FROM managed_jobs"))
        return fail("storage_error", query.lastError().text());
    if (!query.next())
        return fail("storage_error", "The task admission counts could not be read.");
    if (query.value(0).toLongLong() >= 100 || query.value(1).toLongLong() >= 116)
        return fail("resource_exhausted",
                    "The background task queue or nonterminal capacity is full.");
    query.finish();
    QString rootSession = intent.owner.sessionId;
    qint64 rootGeneration = intent.owner.generation;
    QString parentTask;
    query.prepare("SELECT j.root_session_id,j.root_generation,j.task_id,c.child_generation,j.state "
                  "FROM managed_job_children c LEFT JOIN managed_jobs j ON j.task_id=c.task_id "
                  "WHERE c.child_session_id=?");
    query.addBindValue(intent.owner.sessionId);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (query.next()) {
        if (query.value(3).toLongLong() != intent.owner.generation ||
            (query.value(4).toString() != "starting" && query.value(4).toString() != "running"))
            return fail("stale_owner", "The managed child's parent is no longer executing.");
        rootSession = query.value(0).toString();
        rootGeneration = query.value(1).toLongLong();
        parentTask = query.value(2).toString();
    }
    query.finish();
    if (!currentOwner(database_, {rootSession, rootGeneration, {}}))
        return fail("stale_owner", "The root owner generation is no longer current.");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject record{{"task_id", id},
                             {"schema_version", 1},
                             {"owner_session_id", intent.owner.sessionId},
                             {"owner_generation", intent.owner.generation},
                             {"originating_turn_id", intent.owner.turnId},
                             {"root_owner_session_id", rootSession},
                             {"root_owner_generation", rootGeneration},
                             {"parent_task_id", parentTask.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                                     : QJsonValue(parentTask)},
                             {"kind", intent.kind},
                             {"title", intent.title},
                             {"state", "queued"},
                             {"containment_mode", intent.kind == "process"
                                                      ? QJsonValue(intent.containmentMode)
                                                      : QJsonValue(QJsonValue::Null)},
                             {"cleanup_status", "not_required"},
                             {"executor_stopped", true},
                             {"revision", 1},
                             {"created_at", now},
                             {"updated_at", now},
                             {"timeout_ms", intent.timeoutMs},
                             {"execution", intent.executionMetadata},
                             {"permission_approved", false},
                             {"exit_code", QJsonValue::Null},
                             {"error", QJsonValue::Null},
                             {"reason", QJsonValue::Null},
                             {"output_cursor", QJsonValue::Null},
                             {"output_size", 0},
                             {"output_truncated", false},
                             {"completion_event_id", QJsonValue::Null},
                             {"execution_identity", QJsonObject{}},
                             {"completion", QJsonValue::Null}};
    query.prepare(
        "INSERT INTO managed_jobs(task_id,owner_session_id,owner_generation,"
        "client_request_id,spec_fingerprint,record,root_session_id,root_generation,state) "
        "VALUES(?,?,?,?,?,?,?,?,'queued')");
    query.addBindValue(id);
    query.addBindValue(intent.owner.sessionId);
    query.addBindValue(intent.owner.generation);
    query.addBindValue(intent.clientRequestId);
    query.addBindValue(
        remote_job_rows::fingerprint(intent.specFingerprint, intent.runsOn, intent.timeoutMs));
    query.addBindValue(json(record));
    query.addBindValue(rootSession);
    query.addBindValue(rootGeneration);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    QString reserveError;
    if (intent.runsOn &&
        !remote_job_rows::reserve(database_, id, *intent.runsOn, intent.timeoutMs, &reserveError))
        return fail("storage_error", reserveError);
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", record}, {"reused", false}};
}

QJsonObject BackgroundTaskLedger::change(const QString& id, qint64 expectedRevision,
                                         const BackgroundTaskChange& change) {
    return changeImpl(id, expectedRevision, change, {});
}

QJsonObject BackgroundTaskLedger::resolveCleanup(const QString& id, qint64 revision,
                                                 const QString& userClientId) {
    if (userClientId.isEmpty())
        return failure("user_action_required", "Only a local user may acknowledge cleanup.");
    BackgroundTaskChange change;
    change.state = "interrupted";
    change.reason = "cleanup_acknowledged";
    change.cleanupStatus = "unverified";
    change.suppressReason = "cleanup_acknowledged";
    return changeImpl(id, revision, change, userClientId);
}

} // namespace ketplus::daemon
