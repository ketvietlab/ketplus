#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {
QJsonObject BackgroundTaskLedger::list(const BackgroundTaskOwner& owner, const QString& cursor,
                                       int limit) {
    const auto error = [](const QString& code, const QString& message) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", message}};
    };
    if (limit < 1 || limit > 100)
        return error("invalid_request", "Task page size must be between 1 and 100.");
    qint64 after = 0;
    qint64 requestedRevision = 0;
    if (!cursor.isEmpty()) {
        const auto decoded = QByteArray::fromBase64Encoding(
            cursor.toLatin1(),
            QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
        const auto position = QJsonDocument::fromJson(decoded.decoded).object();
        bool sequenceOk = false;
        bool revisionOk = false;
        after = position.value("after").toString().toLongLong(&sequenceOk);
        requestedRevision = position.value("revision").toString().toLongLong(&revisionOk);
        if (!decoded || !sequenceOk || !revisionOk || after <= 0 || requestedRevision <= 0 ||
            position.value("owner") != owner.sessionId ||
            position.value("generation").toInteger() != owner.generation)
            return error("invalid_cursor", "The cursor does not belong to this owner page.");
    }
    if (!database_.transaction())
        return error("storage_error", database_.lastError().text());
    const auto fail = [&](const QString& code, const QString& message) {
        database_.rollback();
        return error(code, message);
    };
    QSqlQuery query(database_);
    query.prepare("SELECT c.revision FROM managed_job_clock c JOIN sessions s "
                  "ON s.id=? AND s.generation=? WHERE c.singleton=1");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    if (!query.next())
        return fail("stale_owner", "The owner generation is no longer current.");
    const qint64 revision = query.value(0).toLongLong();
    query.finish();
    if (requestedRevision && requestedRevision != revision)
        return fail("stale_cursor", "Registry changed; refetch the task snapshot.");
    query.prepare("SELECT task_id,sequence FROM managed_jobs WHERE owner_session_id=? "
                  "AND owner_generation=? AND sequence>? ORDER BY sequence LIMIT ?");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    query.addBindValue(after);
    query.addBindValue(limit + 1);
    if (!query.exec())
        return fail("storage_error", query.lastError().text());
    QList<QPair<QString, qint64>> ids;
    while (query.next())
        ids.append({query.value(0).toString(), query.value(1).toLongLong()});
    query.finish();
    const bool more = ids.size() > limit;
    if (more)
        ids.removeLast();
    QJsonArray tasks;
    for (const auto& [id, sequence] : ids) {
        const auto record = task(id);
        if (record.contains("error_code"))
            return fail(record.value("error_code").toString(),
                        record.value("error_message").toString());
        tasks.append(record);
    }
    query.prepare("SELECT count(*) FROM managed_jobs WHERE owner_session_id=? "
                  "AND owner_generation=? AND holds_slot=1");
    query.addBindValue(owner.sessionId);
    query.addBindValue(owner.generation);
    if (!query.exec() || !query.next())
        return fail("storage_error", query.lastError().text());
    const int activeCount = query.value(0).toInt();
    query.finish();
    if (!database_.commit())
        return fail("storage_error", database_.lastError().text());
    QJsonValue next(QJsonValue::Null);
    if (more) {
        const QJsonObject position{{"owner", owner.sessionId},
                                   {"generation", owner.generation},
                                   {"revision", QString::number(revision)},
                                   {"after", QString::number(ids.last().second)}};
        next = QString::fromLatin1(
            QJsonDocument(position)
                .toJson(QJsonDocument::Compact)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }
    return {{"ok", true},
            {"tasks", tasks},
            {"next_cursor", next},
            {"revision", revision},
            {"active_count", activeCount}};
}
} // namespace ketplus::daemon
