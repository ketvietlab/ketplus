#include "storage/BackgroundTaskLedger.h"

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

QJsonObject BackgroundTaskLedger::nonterminal() const {
    QSqlQuery query(database_);
    if (!query.exec(
            "SELECT task_id FROM managed_jobs WHERE state NOT IN "
            "('completed','failed','cancelled','timed_out','interrupted') ORDER BY sequence"))
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
