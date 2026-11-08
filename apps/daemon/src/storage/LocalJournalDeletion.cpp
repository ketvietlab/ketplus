#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

namespace detail {
QString deletionWorkQuery(bool pendingCloudOnly, bool wrap) {
    return "SELECT deletion,provider_targets,cloud_acked,rowid FROM session_deletion_work WHERE " +
           QString(pendingCloudOnly ? "cloud_acked=0"
                                    : "(provider_targets != '[]' OR cloud_acked=0)=1") +
           (wrap ? " AND rowid<=?" : " AND rowid>?") + " ORDER BY rowid LIMIT ?";
}
} // namespace detail

QJsonArray LocalJournal::deletionWork(const int limit, const bool pendingCloudOnly,
                                      const qint64 afterRow) const {
    const int count = qBound(1, limit, 256);
    const qint64 after = qMax<qint64>(0, afterRow);
    QJsonArray result;
    // The indexed suffix and optional prefix share one result limit. Completed
    // rows do not require a scan or a backlog-sized round-robin sort.
    for (int pass = 0; pass < 2 && result.size() < count; ++pass) {
        if (pass == 1 && after == 0)
            break;
        QSqlQuery query(database_);
        query.prepare(detail::deletionWorkQuery(pendingCloudOnly, pass == 1));
        query.addBindValue(after);
        query.addBindValue(count - result.size());
        if (!query.exec()) {
            setError(query.lastError().text());
            return {};
        }
        while (query.next())
            result.append(QJsonObject{
                {"deletion", QJsonDocument::fromJson(query.value(0).toByteArray()).object()},
                {"provider_targets", QJsonDocument::fromJson(query.value(1).toByteArray()).array()},
                {"cloud_acked", query.value(2).toBool()},
                {"work_id", query.value(3).toLongLong()}});
        if (query.lastError().isValid()) {
            setError(query.lastError().text());
            return {};
        }
    }
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
    if (provider.isEmpty() || threadId.isEmpty())
        return false;
    QSqlQuery query(database_);
    query.prepare(
        "SELECT provider_targets FROM session_deletion_work WHERE session_id=? AND generation=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next())
        return false;
    const auto previous = query.value(0).toString();
    const auto document = QJsonDocument::fromJson(previous.toUtf8());
    if (!document.isArray()) {
        setError("Malformed provider cleanup targets remain pending.");
        return false;
    }
    const auto targets = document.array();
    QJsonArray kept;
    for (const auto& value : targets) {
        const auto target = value.toObject();
        if (target.value("provider") != provider || target.value("thread_id") != threadId)
            kept.append(value);
    }
    query.prepare(
        "UPDATE session_deletion_work SET provider_targets=? WHERE session_id=? AND generation=? "
        "AND provider_targets=?");
    query.addBindValue(QString::fromUtf8(QJsonDocument(kept).toJson(QJsonDocument::Compact)));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(previous);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError("Provider cleanup progress changed or was not stored.");
        return false;
    }
    return true; // Physical removal belongs to bounded metadata maintenance.
}

bool LocalJournal::acknowledgeCloudDeletion(const QString& sessionId, const qint64 generation,
                                            const QString& operationId) {
    if (operationId.isEmpty())
        return false;
    QSqlQuery query(database_);
    query.prepare("SELECT deletion FROM session_deletion_work WHERE session_id=? AND generation=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec() || !query.next())
        return false;
    const auto previous = query.value(0).toString();
    const auto document = QJsonDocument::fromJson(previous.toUtf8());
    if (!document.isObject() || document.object().value("operationId") != operationId)
        return false;
    query.prepare("UPDATE session_deletion_work SET cloud_acked=1 WHERE session_id=? AND "
                  "generation=? AND deletion=?");
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    query.addBindValue(previous);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError("Cloud deletion receipt changed or was not stored.");
        return false;
    }
    return true;
}
} // namespace ketplus::daemon
