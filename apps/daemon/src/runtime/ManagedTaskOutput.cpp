#include "runtime/ManagedTaskRuntime.h"
#include "storage/BackgroundTaskOutput.h"

#include <QFileInfo>
#include <QSqlError>
#include <QUuid>

namespace ketplus::daemon {
QJsonObject ManagedTaskRuntime::output(const QString& id, const BackgroundTaskOwner& owner,
                                       const QString& cursor) const {
    const auto authorized = ledger_.get(id, owner);
    if (!authorized.value("ok").toBool())
        return authorized;
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
                page = output.read(id, cursor);
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
