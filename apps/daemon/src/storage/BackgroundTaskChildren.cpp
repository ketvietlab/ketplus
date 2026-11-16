#include "storage/BackgroundTaskLedger.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <limits>

namespace ketplus::daemon {
QJsonObject BackgroundTaskLedger::reserveChild(const QString& id, qint64 expectedRevision) {
    const auto error = [](const QString& code, const QString& message) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", message}};
    };
    if (!database_.transaction())
        return error("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return error(code, message);
    };
    auto record = task(id);
    if (record.contains("error_code"))
        return fail(record.value("error_code").toString(),
                    record.value("error_message").toString());
    if (record.value("revision").toInteger() != expectedRevision)
        return fail("revision_conflict", "The task revision changed.");
    if (record.value("kind") != "agent" || record.value("state") != "starting" ||
        !record.value("permission_approved").toBool() ||
        expectedRevision == std::numeric_limits<qint64>::max())
        return fail("invalid_transition",
                    "Only an approved, starting agent may reserve its child.");
    QSqlQuery query(database_);
    query.prepare("SELECT 1 FROM sessions o JOIN sessions r WHERE o.id=? AND o.generation=? "
                  "AND r.id=? AND r.generation=?");
    query.addBindValue(record.value("owner_session_id").toString());
    query.addBindValue(record.value("owner_generation").toInteger());
    query.addBindValue(record.value("root_owner_session_id").toString());
    query.addBindValue(record.value("root_owner_generation").toInteger());
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!query.next())
        return fail("stale_owner", "The managed child's owner generation is no longer current.");
    query.finish();
    query.prepare(
        "SELECT child_session_id,child_generation FROM managed_job_children WHERE task_id=?");
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (query.next()) {
        query.finish();
        database_.rollback();
        return {{"ok", true}, {"task", record}, {"reused", true}};
    }
    query.finish();
    if (!record.value("execution_identity").toObject().isEmpty())
        return fail("invalid_execution_identity",
                    "The agent already has another executor identity.");
    const auto child = QUuid::createUuid().toString(QUuid::WithoutBraces);
    query.prepare("INSERT INTO managed_job_children VALUES(?,?,1)");
    query.addBindValue(id);
    query.addBindValue(child);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    record.insert("execution_identity",
                  QJsonObject{{"child_session_id", child}, {"child_generation", 1}});
    record.insert("revision", expectedRevision + 1);
    record.insert("updated_at", QDateTime::currentMSecsSinceEpoch());
    query.prepare("UPDATE managed_jobs SET record=? WHERE task_id=?");
    query.addBindValue(QJsonDocument(record).toJson(QJsonDocument::Compact));
    query.addBindValue(id);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    return {{"ok", true}, {"task", record}, {"reused", false}};
}
} // namespace ketplus::daemon
