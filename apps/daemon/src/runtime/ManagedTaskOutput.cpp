#include "runner/entry/RemoteOutput.h"
#include "runtime/ManagedTaskRuntime.h"
#include "storage/BackgroundTaskOutput.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QUuid>

namespace ketplus::daemon {
bool ManagedTaskRuntime::appendChildOutput(const QString& id, qint64 turn,
                                           const QByteArray& bytes) {
    const auto live = live_.value(id);
    if (!live || live->record.value("kind") != "agent" || turn <= 0)
        return false;
    const auto path = directory_ + '/' + id;
    if (!QDir().mkpath(path) ||
        !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return false;
    const auto connection = QUuid::createUuid().toString();
    bool stored = false;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(path + "/output.sqlite");
        if (database.open()) {
            BackgroundTaskOutput output(database);
            QString error;
            stored = output.initialize(&error) && output.append(id, bytes, &error, turn);
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return stored;
}

QJsonObject ManagedTaskRuntime::output(const QString& id, const BackgroundTaskOwner& owner,
                                       const QString& cursor, bool localAdministration) const {
    const auto authorized = ledger_.get(id, owner, localAdministration);
    if (!authorized.value("ok").toBool())
        return authorized;
    if (const auto remote = runner::entry::remoteOutput(ledger_.database(), id, cursor, authorized))
        return *remote;
    const auto path = directory_ + '/' + id + "/output.sqlite";
    const auto connection = QUuid::createUuid().toString();
    BackgroundOutputPage page;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        if (QFileInfo::exists(path)) {
            database.setDatabaseName(path);
            database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=50");
        } else {
            // Before an executor is launched there is no spool, but cursors still
            // use exactly the same raw-byte protocol as a live spool.
            database.setDatabaseName(":memory:");
        }
        if (!database.open()) {
            page.error = database.lastError().text();
        } else {
            BackgroundTaskOutput output(database);
            if (!QFileInfo::exists(path))
                output.initialize(&page.error);
            if (page.error.isEmpty())
                page =
                    output.read(id, cursor, BackgroundTaskOutput::maximumPageBytes, true,
                                authorized.value("task").toObject().value("ended_at").isDouble());
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (!page.error.isEmpty())
        return {{"ok", false}, {"error_code", "output_unavailable"}, {"error_message", page.error}};
    return {
        {"ok", true},
        {"output", QString::fromUtf8(page.bytes)},
        {"next_cursor", page.nextCursor},
        {"total_bytes", page.totalBytes},
        {"truncated",
         page.truncated || authorized.value("task").toObject().value("output_truncated").toBool()}};
}
} // namespace ketplus::daemon
