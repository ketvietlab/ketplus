#include "storage/JournalFormat.h"
#include "storage/LocalJournal.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

bool LocalJournal::acceptsFormat() {
    // Runs before any schema statement: a newer journal is refused without a single write.
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    const int version = query.value(0).toInt();
    if (version < 0 || version > journal_format::current) {
        setError(QStringLiteral("This journal was written by a newer KetPlus (journal format %1; "
                                "this version opens up to %2). Update KetPlus; the journal "
                                "was not changed.")
                     .arg(version)
                     .arg(journal_format::current));
        return false;
    }
    return true;
}

bool LocalJournal::markFormat() {
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    if (query.value(0).toInt() >= journal_format::current)
        return true;
    query.finish();
    // Only after every migration succeeded, so a failed upgrade is retried at the next open.
    if (!query.exec(QStringLiteral("PRAGMA user_version=%1").arg(journal_format::current))) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

} // namespace ketplus::daemon
