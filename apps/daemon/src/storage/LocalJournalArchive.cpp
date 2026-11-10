#include "storage/LocalJournal.h"

#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {

bool LocalJournal::setSessionArchived(const QString& sessionId, const qint64 generation,
                                      const QString& archivedAt) {
    setError({});
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("UPDATE sessions SET archived_at = ? WHERE id = ? AND generation = ?"));
    // A null QString binds as SQL NULL; the column holds '' when not archived.
    query.addBindValue(archivedAt.isEmpty() ? QStringLiteral("") : archivedAt);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() == 1;
}

} // namespace ketplus::daemon
