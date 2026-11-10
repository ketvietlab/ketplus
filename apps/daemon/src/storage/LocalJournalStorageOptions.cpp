#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
namespace ketplus::daemon {
std::optional<JournalStorageOptions> LocalJournal::storageOptions() const {
    setError({});
    QSqlQuery query(database_);
    // As with the other storage policies, a rejected row stays present and fails closed rather
    // than reading as default revision zero.
    if (!query.exec("SELECT revision,CASE WHEN typeof(policy) IN ('text','blob') "
                    "AND octet_length(policy)<=4096 THEN policy ELSE NULL END,typeof(revision) "
                    "FROM journal_storage_options WHERE singleton=1")) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        if (query.lastError().isValid()) {
            setError(query.lastError().text());
            return std::nullopt;
        }
        return JournalStorageOptions{};
    }
    auto result = JournalStorageOptions::fromJson(
        QJsonDocument::fromJson(query.value(1).toByteArray()).object());
    const auto revision = query.value(0).toLongLong();
    if (!result || query.value(2).toString() != "integer" || revision < 1 ||
        revision > 9007199254740991LL) {
        setError("Stored storage options are invalid; archive expiry is paused.");
        return std::nullopt;
    }
    result->revision = revision;
    return result;
}
bool LocalJournal::setStorageOptions(const JournalStorageOptions& options,
                                     qint64 expectedRevision) {
    setError({});
    if (!JournalStorageOptions::fromJson(options.toJson()) || expectedRevision < 0 ||
        expectedRevision >= 9007199254740991LL) {
        setError("Invalid storage options or revision.");
        return false;
    }
    // Revision zero means no row yet; any other revision must match the stored one.
    QSqlQuery query(database_);
    query.prepare(expectedRevision == 0
                      ? QStringLiteral("INSERT OR IGNORE INTO journal_storage_options"
                                       "(singleton,revision,policy) VALUES(1,?,?)")
                      : QStringLiteral("UPDATE journal_storage_options SET revision=?,policy=? "
                                       "WHERE singleton=1 AND revision=?"));
    query.addBindValue(expectedRevision + 1);
    query.addBindValue(
        QString::fromUtf8(QJsonDocument(options.toJson()).toJson(QJsonDocument::Compact)));
    if (expectedRevision != 0)
        query.addBindValue(expectedRevision);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError("Storage options changed; reload before saving.");
        return false;
    }
    return true;
}
std::optional<QJsonObject> LocalJournal::lastCompaction() const {
    setError({});
    QSqlQuery query(database_);
    if (!query.exec("SELECT completed_at,before_bytes,after_bytes FROM journal_compaction_state "
                    "WHERE singleton=1")) {
        setError(query.lastError().text());
        return std::nullopt;
    }
    if (!query.next())
        return QJsonObject{};
    return QJsonObject{{"completed_at", query.value(0).toString()},
                       {"before_bytes", query.value(1).toLongLong()},
                       {"after_bytes", query.value(2).toLongLong()}};
}
bool LocalJournal::recordCompaction(const QDateTime& completedAt, qint64 beforeBytes,
                                    qint64 afterBytes) {
    setError({});
    QSqlQuery query(database_);
    query.prepare("INSERT INTO journal_compaction_state(singleton,completed_at,before_bytes,"
                  "after_bytes) VALUES(1,?,?,?) ON CONFLICT(singleton) DO UPDATE SET "
                  "completed_at=excluded.completed_at,before_bytes=excluded.before_bytes,"
                  "after_bytes=excluded.after_bytes");
    query.addBindValue(completedAt.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(qMax<qint64>(0, beforeBytes));
    query.addBindValue(qMax<qint64>(0, afterBytes));
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}
} // namespace ketplus::daemon
