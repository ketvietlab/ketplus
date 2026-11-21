#pragma once

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QUuid>
#include <QVariant>

namespace attention_fixture {
enum class IntentLookup { Found, Absent, Error };

// Test-only recovery of an accepted intent when its callback never supplied an ID.
// Read the durable request tuple, which is not exposed in the API projection.
inline IntentLookup resolveCleanupIntent(const QString& databasePath, const QString& owner,
                                         qint64 generation, const QString& requestId,
                                         QString& taskId) {
    if (!taskId.isEmpty())
        return IntentLookup::Found;
    const auto connection = QUuid::createUuid().toString();
    const auto result = [&] {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(databasePath);
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (!db.open())
            return IntentLookup::Error;
        QSqlQuery query(db);
        if (!query.prepare("SELECT task_id FROM managed_jobs WHERE owner_session_id=? "
                           "AND owner_generation=? AND client_request_id=?"))
            return IntentLookup::Error;
        query.addBindValue(owner);
        query.addBindValue(generation);
        query.addBindValue(requestId);
        if (!query.exec())
            return IntentLookup::Error;
        if (!query.next())
            return query.lastError().isValid() ? IntentLookup::Error : IntentLookup::Absent;
        const auto found = query.value(0).toString();
        if (found.isEmpty() || query.next() || query.lastError().isValid())
            return IntentLookup::Error;
        taskId = found;
        return IntentLookup::Found;
    }();
    QSqlDatabase::removeDatabase(connection);
    return result;
}
} // namespace attention_fixture
