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

std::optional<qint64> LocalJournal::reclaimFreePages(const qint64 maximumPages) {
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
    const auto after = space();
    if (!after)
        return std::nullopt;
    return before->freePages - after->freePages;
}

} // namespace ketplus::daemon
