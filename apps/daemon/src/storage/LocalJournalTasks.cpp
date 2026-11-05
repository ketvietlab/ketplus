#include "storage/LocalJournal.h"

#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace ketplus::daemon {

bool LocalJournal::storeBackgroundTasks(const QString& sessionId, const qint64 generation,
                                        const QJsonArray& tasks) {
    QSqlQuery query(database_);
    if (tasks.isEmpty()) {
        query.prepare(QStringLiteral(
            "DELETE FROM background_tasks WHERE session_id = ? AND generation = ?"));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO background_tasks(session_id, generation, tasks) VALUES(?, ?, ?) "
            "ON CONFLICT(session_id, generation) DO UPDATE SET tasks = excluded.tasks"));
        query.addBindValue(sessionId);
        query.addBindValue(generation);
        query.addBindValue(QString::fromUtf8(QJsonDocument(tasks).toJson(QJsonDocument::Compact)));
    }
    if (!query.exec()) {
        setError(query.lastError().text());
        return false;
    }
    return true;
}

QHash<QString, QJsonArray> LocalJournal::backgroundTaskSnapshots() const {
    QHash<QString, QJsonArray> result;
    QSqlQuery query(database_);
    // Only the current generation of a live session: a deleted or reset one has no tasks.
    if (!query.exec(QStringLiteral(
            "SELECT b.session_id, b.tasks FROM background_tasks b JOIN sessions s "
            "ON s.id = b.session_id AND s.generation = b.generation"))) {
        setError(query.lastError().text());
        return result;
    }
    while (query.next())
        result.insert(query.value(0).toString(),
                      QJsonDocument::fromJson(query.value(1).toString().toUtf8()).array());
    return result;
}

} // namespace ketplus::daemon
