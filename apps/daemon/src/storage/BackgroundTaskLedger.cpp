#include "storage/BackgroundTaskLedger.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <limits>
#include <utility>

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
         {"CREATE TABLE IF NOT EXISTS managed_jobs (sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
          "task_id TEXT NOT NULL UNIQUE, "
          "owner_session_id TEXT NOT NULL,owner_generation INTEGER NOT NULL,"
          "root_session_id TEXT NOT NULL,root_generation INTEGER NOT NULL,"
          "state TEXT NOT NULL,holds_slot INTEGER NOT NULL DEFAULT 0,"
          "parked INTEGER NOT NULL DEFAULT 0,resume_ready INTEGER NOT NULL DEFAULT 0,"
          "client_request_id TEXT NOT NULL,spec_fingerprint BLOB NOT NULL,"
          "record BLOB NOT NULL,UNIQUE(owner_session_id,owner_generation,client_request_id))",
          "CREATE INDEX IF NOT EXISTS managed_jobs_slots ON "
          "managed_jobs(holds_slot,root_session_id,root_generation)",
          "CREATE TABLE IF NOT EXISTS managed_job_children(task_id TEXT PRIMARY KEY,"
          "child_session_id TEXT NOT NULL UNIQUE,child_generation INTEGER NOT NULL)",
          "CREATE TABLE IF NOT EXISTS managed_job_outbox (event_id TEXT PRIMARY KEY,"
          "task_id TEXT NOT NULL UNIQUE,owner_session_id TEXT NOT NULL,"
          "owner_generation INTEGER NOT NULL,payload BLOB NOT NULL,status TEXT NOT NULL,reason "
          "TEXT)",
          "CREATE TABLE IF NOT EXISTS managed_job_wakes (event_id TEXT PRIMARY KEY,"
          "owner_session_id TEXT NOT NULL,owner_generation INTEGER NOT NULL,"
          "turn_record BLOB NOT NULL,state TEXT NOT NULL)"}) {
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

QJsonObject BackgroundTaskLedger::get(const QString& taskId,
                                      const BackgroundTaskOwner& owner) const {
    if (!currentOwner(database_, owner))
        return failure("stale_owner", "The owner generation is no longer current.");
    auto record = task(taskId);
    if (record.contains("error_code"))
        return record;
    if (!sameOwner(record, owner))
        return failure("task_not_found", "This task does not belong to this owner generation.");
    return {{"ok", true}, {"task", record}};
}

QJsonObject BackgroundTaskLedger::create(const BackgroundTaskIntent& intent) {
    if (intent.owner.sessionId.isEmpty() || intent.owner.generation <= 0 ||
        intent.clientRequestId.isEmpty() || intent.specFingerprint.isEmpty() ||
        (intent.kind != "process" && intent.kind != "agent") || intent.timeoutMs <= 0)
        return failure("invalid_request", "Incomplete accepted task intent.");
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
        const bool matches = query.value(1).toByteArray() == intent.specFingerprint;
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
                  "FROM managed_job_children c JOIN managed_jobs j ON j.task_id=c.task_id "
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
                                                      ? QJsonValue("native_best_effort")
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
    query.addBindValue(intent.specFingerprint);
    query.addBindValue(json(record));
    query.addBindValue(rootSession);
    query.addBindValue(rootGeneration);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
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
    else if (change.state == "running") {
        const auto identity = change.executionIdentity;
        const bool processIdentity =
            !identity.value("process_birth_identity").toString().isEmpty() &&
            !identity.value("executor_lease_identity").toString().isEmpty();
        const bool childIdentity = !identity.value("child_session_id").toString().isEmpty() &&
                                   identity.value("child_generation").toInteger() > 0;
        allowed = previous == "starting" && change.handshakeCommitted &&
                  (record.value("kind") == "process" ? processIdentity : childIdentity);
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
