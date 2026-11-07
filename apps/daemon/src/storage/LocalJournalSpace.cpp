#include "storage/LocalJournal.h"

#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QVariant>

namespace ketplus::daemon {

std::optional<journal_space::Usage> LocalJournal::space() const {
    QSqlQuery query(database_);
    qint64 values[4] = {};
    const char* pragmas[] = {"page_size", "page_count", "freelist_count", "auto_vacuum"};
    for (int index = 0; index < 4; ++index) {
        if (!query.exec(QStringLiteral("PRAGMA %1").arg(QLatin1String(pragmas[index]))) ||
            !query.next()) {
            setError(query.lastError().text());
            return std::nullopt;
        }
        values[index] = query.value(0).toLongLong();
    }
    return journal_space::Usage{values[0], values[1], values[2], int(values[3])};
}

std::optional<QJsonObject> LocalJournal::diskUsage() const {
    setError({});
    QSqlQuery query(database_);
    if (!query.exec("SELECT file FROM pragma_database_list WHERE name='main'") || !query.next()) {
        setError("Journal file location is unavailable.");
        return std::nullopt;
    }
    const QString path = query.value(0).toString();
    query.finish();
    const QFileInfo database(path);
    if (path.isEmpty() || !database.isFile() || !database.isReadable()) {
        setError("Journal file measurements are unavailable.");
        return std::nullopt;
    }
    QJsonObject result;
    qint64 total = 0;
    const QStringList suffixes{QString{}, "-wal", "-shm"};
    const QStringList keys{"database_file_bytes", "wal_file_bytes", "shared_memory_file_bytes"};
    for (qsizetype i = 0; i < suffixes.size(); ++i) {
        const QFileInfo file(path + suffixes.at(i));
        if ((i == 0 && !file.exists()) ||
            (file.exists() && (!file.isFile() || !file.isReadable()))) {
            setError("A journal sidecar cannot be measured.");
            return std::nullopt;
        }
        const qint64 bytes = file.exists() ? file.size() : 0;
        if (bytes < 0 || bytes > 9007199254740991LL - total) {
            setError("Journal file measurements are out of range.");
            return std::nullopt;
        }
        result.insert(keys.at(i), bytes);
        total += bytes;
    }
    result.insert("total_file_bytes", total);
    result.insert("sampled_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const QStorageInfo storage(database.absoluteFilePath());
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() >= 0 &&
        storage.bytesAvailable() <= 9007199254740991LL)
        result.insert("filesystem_available_bytes", storage.bytesAvailable());
    // Missing filesystem availability is unknown, never a fabricated zero/healthy state.
    return result;
}

bool LocalJournal::checkpointReclaimedPages() {
    // The smaller size is committed to the write-ahead log; the file shrinks when a checkpoint
    // copies all of it back. A passive checkpoint never waits: when a reader holds frames back it
    // stops and the next call tries again.
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("PRAGMA wal_checkpoint(PASSIVE)")) || !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    reclaimCheckpointPending_ =
        query.value(0).toInt() != 0 || query.value(1).toInt() != query.value(2).toInt();
    query.finish();
    // Emptying the log as well waits for anyone still reading it, so it runs without the busy
    // timeout. Skipped, it costs nothing: the file has shrunk and the next write reuses the log.
    if (!query.exec(QStringLiteral("PRAGMA busy_timeout")) || !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    const int busyTimeout = query.value(0).toInt();
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA busy_timeout=0"))) {
        setError(query.lastError().text());
        return false;
    }
    query.finish();
    // Busy is a result here, not a failure.
    query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(busyTimeout))) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

std::optional<qint64> LocalJournal::reclaimFreePages(const qint64 maximumPages) {
    if (reclaimCheckpointPending_ && !checkpointReclaimedPages())
        return std::nullopt;
    const auto before = space();
    if (!before)
        return std::nullopt;
    if (before->autoVacuum != 2 || before->freePages < minimumReclaimPages)
        return 0;
    // QSQLITE steps a statement once, and SQLite returns one page per step of
    // incremental_vacuum: one page per execution, all of them in one transaction.
    const qint64 pages = qMin(maximumPages, before->freePages);
    if (!database_.transaction()) {
        setError(database_.lastError().text());
        return std::nullopt;
    }
    QSqlQuery query(database_);
    if (!query.prepare(QStringLiteral("PRAGMA incremental_vacuum(1)"))) {
        setError(query.lastError().text());
        database_.rollback();
        return std::nullopt;
    }
    for (qint64 page = 0; page < pages; ++page) {
        if (!query.exec()) {
            setError(query.lastError().text());
            query.finish();
            database_.rollback();
            return std::nullopt;
        }
    }
    query.finish();
    if (!database_.commit()) {
        setError(database_.lastError().text());
        database_.rollback();
        return std::nullopt;
    }
    if (!checkpointReclaimedPages())
        return std::nullopt;
    const auto after = space();
    if (!after)
        return std::nullopt;
    return before->freePages - after->freePages;
}

} // namespace ketplus::daemon
