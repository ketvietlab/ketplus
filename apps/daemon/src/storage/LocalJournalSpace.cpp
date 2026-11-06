#include "storage/LocalJournal.h"

#include <QSqlError>
#include <QSqlQuery>
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
