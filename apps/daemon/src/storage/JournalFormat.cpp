#include "storage/JournalFormat.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <optional>

namespace ketplus::daemon::journal_format {

namespace {
QString unsupportedMessage(const int version) {
    return QStringLiteral("This journal was written by a newer KetPlus (journal format %1; this "
                          "version opens up to %2). Update KetPlus; the journal was not changed.")
        .arg(version)
        .arg(current);
}
} // namespace

Status check(const QString& path, QString* error) {
    if (!QFileInfo::exists(path))
        return Status::Supported;
    const QString name = QStringLiteral("ketplus-journal-format-%1")
                             .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::optional<int> version;
    QString failure;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next())
                version = query.value(0).toInt();
            else
                failure = query.lastError().text();
        } else {
            failure = database.lastError().text();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    if (!version) {
        *error = QStringLiteral("The journal format could not be read: %1").arg(failure);
        return Status::Unreadable;
    }
    if (!supported(*version)) {
        *error = unsupportedMessage(*version);
        return Status::Unsupported;
    }
    return Status::Supported;
}

} // namespace ketplus::daemon::journal_format
