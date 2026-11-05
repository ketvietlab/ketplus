#include "app/RepositoryMetadataStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QVariant>

namespace ketplus {
namespace {
class Connection final {
  public:
    explicit Connection(const QString& path, bool write) {
        if (!write && !QFileInfo::exists(path))
            return;
        if (write && !QDir().mkpath(QFileInfo(path).absolutePath()))
            return;
        name_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        db_.setDatabaseName(path);
        // Never wait for a cache lock on the GUI thread. A miss is harmless.
        db_.setConnectOptions(write ? "QSQLITE_BUSY_TIMEOUT=100"
                                    : "QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=0");
        if (!db_.open())
            return;
        if (write) {
            QSqlQuery query(db_);
            query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            ready_ = query.exec(
                QStringLiteral("CREATE TABLE IF NOT EXISTS repository_metadata ("
                               "path TEXT PRIMARY KEY, identity TEXT NOT NULL, root TEXT NOT NULL, "
                               "verified_at INTEGER NOT NULL)"));
        } else
            ready_ = true;
    }
    ~Connection() {
        db_ = QSqlDatabase();
        if (!name_.isEmpty())
            QSqlDatabase::removeDatabase(name_);
    }
    QSqlDatabase db_;
    bool ready_{false};

  private:
    QString name_;
};
} // namespace

RepositoryMetadataStore::RepositoryMetadataStore(QString databasePath)
    : databasePath_(
          databasePath.isEmpty()
              ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                    .filePath(QStringLiteral("repository-metadata.sqlite"))
              : std::move(databasePath)) {}

std::optional<RepositoryMetadata> RepositoryMetadataStore::lookup(const QString& path) const {
    const auto found = lookupAll({path});
    const auto metadata = found.constFind(path);
    if (metadata == found.cend())
        return std::nullopt;
    return *metadata;
}

QHash<QString, RepositoryMetadata>
RepositoryMetadataStore::lookupAll(const QStringList& paths) const {
    QHash<QString, RepositoryMetadata> result;
    QStringList existing;
    for (const QString& path : paths) {
        if (!path.isEmpty() && !existing.contains(path) && QFileInfo(path).isDir())
            existing.append(path);
    }
    if (existing.isEmpty())
        return result;
    // One connection for the whole batch: opening SQLite costs more than the queries.
    Connection connection(databasePath_, false);
    if (!connection.ready_)
        return result;
    QSqlQuery query(connection.db_);
    query.prepare(QStringLiteral("SELECT identity, root FROM repository_metadata WHERE path=?"));
    for (const QString& path : std::as_const(existing)) {
        query.bindValue(0, path);
        if (!query.exec() || !query.next())
            continue;
        RepositoryMetadata metadata{path, query.value(0).toString(), query.value(1).toString()};
        query.finish();
        // Never present a cached repo whose checkout or Git metadata disappeared.
        if (QFileInfo(metadata.root).isDir() && QFileInfo::exists(metadata.identity))
            result.insert(path, metadata);
    }
    return result;
}

void RepositoryMetadataStore::save(const RepositoryMetadata& metadata) const {
    Connection connection(databasePath_, true);
    if (!connection.ready_)
        return;
    QSqlQuery query(connection.db_);
    query.prepare(QStringLiteral(
        "INSERT INTO repository_metadata(path, identity, root, verified_at) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET identity=excluded.identity, root=excluded.root, "
        "verified_at=excluded.verified_at"));
    query.addBindValue(metadata.path);
    query.addBindValue(metadata.identity);
    query.addBindValue(metadata.root);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.exec();
}

void RepositoryMetadataStore::remove(const QString& path) const {
    Connection connection(databasePath_, true);
    if (!connection.ready_)
        return;
    QSqlQuery query(connection.db_);
    query.prepare(QStringLiteral("DELETE FROM repository_metadata WHERE path=?"));
    query.addBindValue(path);
    query.exec();
}

} // namespace ketplus
