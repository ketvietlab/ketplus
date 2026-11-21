#include "storage/BackgroundTaskLedger.h"
#include "storage/RemoteJobRows.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <limits>

namespace ketplus::daemon {
namespace {
QJsonObject failure(const QString& code, const QString& message) {
    return {{"ok", false}, {"error_code", code}, {"error_message", message}};
}
QByteArray encoded(const QJsonObject& record) {
    return QJsonDocument(record).toJson(QJsonDocument::Compact);
}
} // namespace

QJsonObject BackgroundTaskLedger::claimNext() {
    if (!database_.transaction())
        return failure("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return failure(code, message);
    };
    QSqlQuery query(database_);
    if (!query.exec("SELECT count(*) FROM managed_jobs WHERE holds_slot=1") || !query.next())
        return fail("storage_error", query.lastError().text());
    const bool full = query.value(0).toLongLong() >= 16;
    query.finish();
    if (full) {
        database_.rollback();
        return {{"ok", true}, {"task", QJsonValue::Null}};
    }
    query.prepare(
        "SELECT j.task_id,j.record,j.parked FROM managed_jobs j "
        "JOIN sessions s ON s.id=j.owner_session_id AND s.generation=j.owner_generation "
        "JOIN sessions r ON r.id=j.root_session_id AND r.generation=j.root_generation "
        "WHERE NOT EXISTS(SELECT 1 FROM remote_jobs x WHERE x.task_id=j.task_id) AND "
        "((j.state='queued' AND json_extract(j.record,'$.permission_approved')=1) "
        "OR (j.parked=1 AND j.resume_ready=1 AND j.state='running')) AND "
        "json_extract(j.record,'$.created_at')+json_extract(j.record,'$.timeout_ms')>? AND "
        "(SELECT count(*) FROM managed_jobs active WHERE active.holds_slot=1 "
        "AND active.root_session_id=j.root_session_id AND "
        "active.root_generation=j.root_generation)<4 "
        "ORDER BY j.sequence LIMIT 1");
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!query.next()) {
        database_.rollback();
        return {{"ok", true}, {"task", QJsonValue::Null}};
    }
    const QString id = query.value(0).toString();
    auto record = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
    const bool resume = query.value(2).toBool();
    query.finish();
    const auto revision = record.value("revision").toInteger();
    if (revision <= 0 || revision == std::numeric_limits<qint64>::max())
        return fail("revision_exhausted", "The task revision cannot be advanced.");
    record.insert("revision", revision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    record.insert("state", resume ? "running" : "starting");
    record.insert("parked", false);
    record.insert("slot_held", true);
    query.prepare("UPDATE managed_jobs SET record=?,state=?,holds_slot=1,parked=0,resume_ready=0 "
                  "WHERE task_id=? AND holds_slot=0");
    query.addBindValue(encoded(record));
    query.addBindValue(record.value("state").toString());
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (query.numRowsAffected() != 1)
        return fail("revision_conflict", "The task slot was claimed concurrently.");
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", record}, {"resume", resume}};
}

QJsonObject BackgroundTaskLedger::park(const QString& id, qint64 expectedRevision,
                                       bool noInferenceRunning) {
    if (!noInferenceRunning)
        return failure("execution_unverified",
                       "An agent slot cannot be released during inference.");
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
    if (record.value("revision").toInteger() != expectedRevision)
        return fail("revision_conflict", "The task revision changed.");
    if (record.value("kind") != "agent" || record.value("state") != "running" ||
        !record.value("slot_held").toBool() ||
        expectedRevision == std::numeric_limits<qint64>::max())
        return fail("invalid_transition", "Only a verified, running agent may park its slot.");
    record.insert("revision", expectedRevision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    record.insert("parked", true);
    record.insert("slot_held", false);
    QSqlQuery query(database_);
    query.prepare(
        "UPDATE managed_jobs SET record=?,parked=1,holds_slot=0,resume_ready=0 WHERE task_id=?");
    query.addBindValue(encoded(record));
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", record}};
}

QJsonObject BackgroundTaskLedger::readyToResume(const QString& id, qint64 expectedRevision) {
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
    if (record.value("revision").toInteger() != expectedRevision)
        return fail("revision_conflict", "The task revision changed.");
    if (record.value("kind") != "agent" || record.value("state") != "running" ||
        !record.value("parked").toBool() || expectedRevision == std::numeric_limits<qint64>::max())
        return fail("invalid_transition", "Only a parked agent may request a resume slot.");
    record.insert("revision", expectedRevision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    QSqlQuery query(database_);
    query.prepare("UPDATE managed_jobs SET record=?,resume_ready=1 WHERE task_id=? AND parked=1");
    query.addBindValue(encoded(record));
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", record}};
}

} // namespace ketplus::daemon
