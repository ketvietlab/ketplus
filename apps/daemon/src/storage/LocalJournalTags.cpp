#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace ketplus::daemon {

bool LocalJournal::setSessionTags(const QString& sessionId, const qint64 generation,
                                  const QStringList& tags) {
    setError({});
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE sessions SET tags = ? WHERE id = ? AND generation = ?"));
    query.addBindValue(QString::fromUtf8(
        QJsonDocument(QJsonArray::fromStringList(tags)).toJson(QJsonDocument::Compact)));
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool LocalJournal::setSessionAwaitingReply(const QString& sessionId, const qint64 generation,
                                           const bool awaiting) {
    setError({});
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("UPDATE sessions SET awaiting_reply = ? WHERE id = ? AND generation = ?"));
    query.addBindValue(awaiting ? 1 : 0);
    query.addBindValue(sessionId);
    query.addBindValue(generation);
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() == 1;
}

} // namespace ketplus::daemon
