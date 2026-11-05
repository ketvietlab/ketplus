#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

QJsonArray LocalJournal::deletionWork(const int limit, const bool pendingCloudOnly,
                                      const qint64 afterRow) const {
    QSqlQuery query(database_);
    query.prepare(
        "SELECT deletion,provider_targets,cloud_acked,rowid FROM session_deletion_work WHERE " +
        QString(pendingCloudOnly ? "cloud_acked=0 "
                                 : "(provider_targets != '[]' OR cloud_acked=0) ") +
        "ORDER BY (rowid <= ?), rowid LIMIT ?");
    query.addBindValue(afterRow);
    query.addBindValue(qBound(1, limit, 256));
    QJsonArray result;
    if (query.exec())
        while (query.next())
            result.append(QJsonObject{
                {"deletion", QJsonDocument::fromJson(query.value(0).toByteArray()).object()},
                {"provider_targets", QJsonDocument::fromJson(query.value(1).toByteArray()).array()},
                {"cloud_acked", query.value(2).toBool()},
                {"work_id", query.value(3).toLongLong()}});
    return result;
}

QJsonArray LocalJournal::sessionTombstones() const {
    QSqlQuery query(database_);
    QJsonArray result;
    if (query.exec("SELECT session_id,generation FROM session_tombstones"))
        while (query.next())
            result.append(QJsonObject{{"session_id", query.value(0).toString()},
                                      {"session_generation", query.value(1).toLongLong()}});
    return result;
}

bool LocalJournal::completeProviderCleanup(const QString& sessionId, const qint64 generation,
                                           const QString& provider, const QString& threadId) {
    QSqlQuery query(database_);
    query.prepare(
        "SELECT provider_targets FROM session_deletion_work WHERE session_id=? AND generation=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next())
        return false;
    const auto targets = QJsonDocument::fromJson(query.value(0).toByteArray()).array();
    QJsonArray kept;
    for (const auto& value : targets) {
        const auto target = value.toObject();
        if (target.value("provider") != provider || target.value("thread_id") != threadId)
            kept.append(value);
    }
    query.prepare(
        "UPDATE session_deletion_work SET provider_targets=? WHERE session_id=? AND generation=?");
    query.addBindValue(QString::fromUtf8(QJsonDocument(kept).toJson(QJsonDocument::Compact)));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec())
        return false;
    return query.exec(
        "DELETE FROM session_deletion_work WHERE provider_targets='[]' AND cloud_acked=1");
}

bool LocalJournal::acknowledgeCloudDeletion(const QString& sessionId, const qint64 generation,
                                            const QString& operationId) {
    QSqlQuery query(database_);
    query.prepare("SELECT deletion FROM session_deletion_work WHERE session_id=? AND generation=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next() ||
        QJsonDocument::fromJson(query.value(0).toByteArray()).object().value("operationId") !=
            operationId)
        return false;
    query.prepare(
        "UPDATE session_deletion_work SET cloud_acked=1 WHERE session_id=? AND generation=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec())
        return false;
    return query.exec(
        "DELETE FROM session_deletion_work WHERE provider_targets='[]' AND cloud_acked=1");
}
} // namespace ketplus::daemon
