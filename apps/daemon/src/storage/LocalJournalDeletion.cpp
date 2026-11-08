#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QVariant>

namespace ketplus::daemon {

namespace detail {
QString deletionWorkQuery(bool pendingCloudOnly, bool wrap) {
    return "SELECT deletion,provider_targets,cloud_acked,rowid FROM session_deletion_work WHERE " +
           QString(pendingCloudOnly ? "cloud_acked=0"
                                    : "(provider_targets != '[]' OR cloud_acked=0)=1") +
           (wrap ? " AND rowid<=?" : " AND rowid>?") + " ORDER BY rowid LIMIT ?";
}
QString sessionTombstonesQuery(const bool continuation) {
    // Guard storage types and bytes before Qt materializes a corrupted identity.
    // Keep validation out of WHERE so malformed rows cannot disappear from a page.
    return "SELECT CASE WHEN typeof(session_id)='text' AND octet_length(session_id) BETWEEN 1 "
           "AND 1024 THEN CAST(session_id AS BLOB) END, CASE WHEN typeof(generation)='integer' AND "
           "generation>0 THEN generation END FROM session_tombstones " +
           QString(continuation ? "WHERE (session_id,generation)>(?,?) " : "") +
           "ORDER BY session_id,generation LIMIT ?";
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

std::optional<SessionTombstonePage> LocalJournal::sessionTombstones(const QJsonObject& after,
                                                                    const bool legacy) const {
    const auto id = after.value("session_id").toString();
    const auto generation = after.value("session_generation").toInteger(-1);
    if (!after.isEmpty() &&
        (after.size() != 2 || id.isEmpty() || id.toUtf8().size() > 1024 || generation < 1)) {
        setError("Invalid session tombstone cursor.");
        return std::nullopt;
    }
    constexpr int pageRows = 128;
    const int rows = legacy ? 32768 : pageRows;
    const qsizetype bytes = legacy ? 960 * 1024 : 64 * 1024;
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    query.prepare(detail::sessionTombstonesQuery(!after.isEmpty()));
    if (!after.isEmpty()) {
        query.addBindValue(id);
        query.addBindValue(generation);
    }
    query.addBindValue(rows + 1);
    if (!query.exec()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    SessionTombstonePage result;
    qsizetype used = 2;
    while (query.next()) {
        if (query.value(0).isNull() || query.value(1).isNull()) {
            setError("Malformed session tombstone remains in the journal.");
            return std::nullopt;
        }
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless |
                                                         QStringConverter::Flag::ConvertInitialBom);
        const auto identity = query.value(0).toByteArray();
        const QString sessionId = decoder(identity);
        if (decoder.hasError() || sessionId.toUtf8() != identity) {
            setError("Malformed UTF-8 session tombstone remains in the journal.");
            return std::nullopt;
        }
        const QJsonObject row{{"session_id", sessionId},
                              {"session_generation", query.value(1).toLongLong()}};
        const auto size = QJsonDocument(row).toJson(QJsonDocument::Compact).size() + 1;
        if (result.tombstones.size() == rows || used + size > bytes) {
            result.next = result.tombstones.last().toObject();
            break;
        }
        result.tombstones.append(row);
        used += size;
    }
    if (query.lastError().isValid()) {
        setError(query.lastError().text());
        return std::nullopt;
    }
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
