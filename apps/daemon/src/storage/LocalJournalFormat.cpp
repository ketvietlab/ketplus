#include "storage/JournalFormat.h"
#include "storage/LocalJournal.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

bool LocalJournal::acceptsFormat() {
    // The read-only preflight in the constructor ran before this connection opened; this repeats
    // it on the open connection, in case the file changed in between.
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        setError(query.lastError().text());
        return false;
    }
    openedFormat_ = query.value(0).toInt();
    if (const int version = openedFormat_; !journal_format::supported(version)) {
        setError(QStringLiteral("This journal was written by a newer KetPlus (journal format %1; "
                                "this version opens up to %2). Update KetPlus.")
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
