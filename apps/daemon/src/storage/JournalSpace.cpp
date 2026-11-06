#include "storage/JournalSpace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUuid>
#include <QVariant>

#include <optional>

#if defined(Q_OS_WIN)
#include <io.h>
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ketplus::daemon::journal_space {
namespace {

QString compactPath(const QString& path) { return path + QStringLiteral(".compact"); }

// A connection of its own, removed when it goes out of scope.
class Connection final {
  public:
    explicit Connection(const QString& path)
        : name_(QStringLiteral("ketplus-journal-space-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        database_.open();
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { close(); }

    void close() {
        if (name_.isEmpty())
            return;
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(name_);
        name_.clear();
    }
    [[nodiscard]] QSqlDatabase& database() { return database_; }

  private:
    QString name_;
    QSqlDatabase database_;
};

std::optional<QVariant> scalar(QSqlDatabase& database, const QString& statement, QString* error) {
    QSqlQuery query(database);
    if (!query.exec(statement) || !query.next()) {
        *error = query.lastError().text();
        if (error->isEmpty())
            *error = QStringLiteral("%1 returned no row.").arg(statement);
        return std::nullopt;
    }
    return query.value(0);
}

std::optional<Usage> readUsage(QSqlDatabase& database, QString* error) {
    const auto pageSize = scalar(database, QStringLiteral("PRAGMA page_size"), error);
    const auto pageCount = scalar(database, QStringLiteral("PRAGMA page_count"), error);
    const auto freePages = scalar(database, QStringLiteral("PRAGMA freelist_count"), error);
    const auto autoVacuum = scalar(database, QStringLiteral("PRAGMA auto_vacuum"), error);
    if (!pageSize || !pageCount || !freePages || !autoVacuum)
        return std::nullopt;
    return Usage{pageSize->toLongLong(), pageCount->toLongLong(), freePages->toLongLong(),
                 autoVacuum->toInt()};
}

void removeCompactCopy(const QString& path) {
    const QString compact = compactPath(path);
    for (const QString& suffix :
         {QString(), QStringLiteral("-journal"), QStringLiteral("-wal"), QStringLiteral("-shm")})
        QFile::remove(compact + suffix);
}

bool syncFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
#if defined(Q_OS_WIN)
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))) != 0;
#else
    return ::fsync(file.handle()) == 0;
#endif
}

// The rename survives a power loss only once its directory is synced.
void syncDirectory([[maybe_unused]] const QString& directory) {
#if !defined(Q_OS_WIN)
    const int descriptor = ::open(QFile::encodeName(directory).constData(), O_RDONLY);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
#endif
}

// Replaces `target` in one step: a reader sees the old file or the new one, never neither.
bool replaceFile(const QString& source, const QString& target) {
#if defined(Q_OS_WIN)
    return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                       reinterpret_cast<LPCWSTR>(target.utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(QFile::encodeName(source).constData(),
                       QFile::encodeName(target).constData()) == 0;
#endif
}

MigrationResult failed(MigrationResult result, const QString& path, const QString& error) {
    removeCompactCopy(path);
    result.outcome = Migration::Failed;
    result.error = error;
    return result;
}

} // namespace

qint64 migrationMargin(const Usage& usage) {
    constexpr qint64 minimumMargin = 256LL * 1024 * 1024;
    return qMax(minimumMargin, usage.keptBytes() / 10);
}

MigrationResult migrateToIncrementalVacuum(const QString& path) {
    MigrationResult result;
    // An earlier attempt stopped before its rename: the original is the journal.
    removeCompactCopy(path);
    if (!QFileInfo::exists(path))
        return result;

    QString error;
    Connection source(path);
    if (!source.database().isOpen())
        return failed(result, path, source.database().lastError().text());
    const auto before = readUsage(source.database(), &error);
    if (!before)
        return failed(result, path, error);
    result.before = *before;
    result.after = *before;
    if (before->autoVacuum == 2)
        return result;

    const qint64 available = QStorageInfo(QFileInfo(path).absolutePath()).bytesAvailable();
    if (available < before->keptBytes() + migrationMargin(*before)) {
        result.outcome = Migration::Deferred;
        result.error = QStringLiteral("%1 bytes free; the compacted journal needs %2.")
                           .arg(available)
                           .arg(before->keptBytes() + migrationMargin(*before));
        return result;
    }

    // Everything in the write-ahead log goes into the main file first, so nothing is left in a
    // log that would no longer match the file it sits beside after the rename.
    QSqlQuery checkpoint(source.database());
    if (!checkpoint.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) || !checkpoint.next())
        return failed(result, path, checkpoint.lastError().text());
    if (checkpoint.value(0).toInt() != 0)
        return failed(result, path, QStringLiteral("The journal is in use."));
    checkpoint.finish();
    const auto tables =
        scalar(source.database(), QStringLiteral("SELECT count(*) FROM sqlite_schema"), &error);
    if (!tables)
        return failed(result, path, error);

    // The copy takes the pending auto_vacuum; the original is not changed.
    QSqlQuery vacuum(source.database());
    if (!vacuum.exec(QStringLiteral("PRAGMA auto_vacuum = INCREMENTAL")) ||
        !vacuum.prepare(QStringLiteral("VACUUM INTO ?")))
        return failed(result, path, vacuum.lastError().text());
    vacuum.addBindValue(compactPath(path));
    if (!vacuum.exec())
        return failed(result, path, vacuum.lastError().text());
    vacuum.finish();
    source.close();
    for (const QString& suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QFileInfo sidecar(path + suffix);
        if (sidecar.exists() && suffix == QStringLiteral("-wal") && sidecar.size() > 0)
            return failed(result, path, QStringLiteral("The journal was written while copied."));
        QFile::remove(sidecar.filePath());
    }

    {
        Connection copy(compactPath(path));
        if (!copy.database().isOpen())
            return failed(result, path, copy.database().lastError().text());
        const auto check = scalar(copy.database(), QStringLiteral("PRAGMA quick_check"), &error);
        if (!check || check->toString() != QStringLiteral("ok"))
            return failed(result, path,
                          error.isEmpty() ? QStringLiteral("The compacted journal failed its "
                                                           "check: %1")
                                                .arg(check ? check->toString() : QString())
                                          : error);
        const auto copiedTables =
            scalar(copy.database(), QStringLiteral("SELECT count(*) FROM sqlite_schema"), &error);
        const auto after = readUsage(copy.database(), &error);
        if (!copiedTables || !after)
            return failed(result, path, error);
        if (copiedTables->toLongLong() != tables->toLongLong() || after->autoVacuum != 2)
            return failed(result, path,
                          QStringLiteral("The compacted journal does not match the original."));
        result.after = *after;
    }
#if !defined(Q_OS_WIN)
    QFile::setPermissions(compactPath(path), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    if (!syncFile(compactPath(path)))
        return failed(result, path, QStringLiteral("The compacted journal could not be synced."));
    if (!replaceFile(compactPath(path), path))
        return failed(result, path,
                      QStringLiteral("The compacted journal could not replace the original."));
    syncDirectory(QFileInfo(path).absolutePath());
    result.outcome = Migration::Migrated;
    return result;
}

} // namespace ketplus::daemon::journal_space
