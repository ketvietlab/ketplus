#include "storage/JournalSpace.h"

#include "storage/JournalFormat.h"

#include <QDir>
#include <QDirIterator>
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

QString compactDirectory(const QString& path) {
    // The VACUUM destination is a literal absolute filename, never a SQLite URI.
    return QFileInfo(path).absoluteFilePath() + QStringLiteral(".compact-private");
}
QString compactPath(const QString& path) {
    return compactDirectory(path) + QStringLiteral("/journal.sqlite");
}
constexpr auto privateDirectory =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr auto groupOrOther = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                              QFileDevice::ExeGroup | QFileDevice::ReadOther |
                              QFileDevice::WriteOther | QFileDevice::ExeOther;

// A connection of its own, removed when it goes out of scope.
class Connection final {
  public:
    explicit Connection(const QString& path, const QString& options = {})
        : name_(QStringLiteral("ketplus-journal-space-%1")
                    .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        database_.setConnectOptions(options);
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

bool removeCompactCopy(const QString& path) {
    // Legacy siblings predate private staging. Remove only regular known artifacts.
    const QStringList suffixes{QString(), "-journal", "-wal", "-shm"};
    for (const auto& suffix : suffixes) {
        const QFileInfo legacy(path + ".compact" + suffix);
        if (!legacy.exists() && !legacy.isSymLink())
            continue;
        if (!legacy.isFile() || legacy.isSymLink() || !QFile::remove(legacy.filePath()))
            return false;
    }
    const QFileInfo directory(compactDirectory(path));
    if (!directory.exists() && !directory.isSymLink())
        return true;
    if (!directory.isDir() || directory.isSymLink())
        return false;
#if !defined(Q_OS_WIN)
    if (directory.ownerId() != ::geteuid() || (directory.permissions() & groupOrOther))
        return false;
#endif
    // At most four known files; never recurse or enumerate an unbounded foreign directory.
    QStringList files;
    {
        QDirIterator entries(directory.filePath(),
                             QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (entries.hasNext()) {
            entries.next();
            const auto entry = entries.fileInfo();
            if (files.size() >= 4 || !entry.isFile() || entry.isSymLink() ||
                !suffixes.contains(entry.fileName().mid(QStringLiteral("journal.sqlite").size())) ||
                !entry.fileName().startsWith("journal.sqlite"))
                return false;
            files.append(entry.filePath());
        }
    }
    for (const auto& file : files)
        if (!QFile::remove(file))
            return false;
    return QDir().rmdir(directory.filePath());
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
    const bool removed = removeCompactCopy(path);
    result.outcome = Migration::Failed;
    result.error = error;
    if (!removed)
        result.error += QStringLiteral(" Migration artifacts were preserved for repair.");
    return result;
}

} // namespace

qint64 migrationMargin(const Usage& usage) {
    constexpr qint64 minimumMargin = 256LL * 1024 * 1024;
    return qMax(minimumMargin, usage.keptBytes() / 10);
}

MigrationResult migrateToIncrementalVacuum(const QString& path) {
    MigrationResult result;
    // Before cleanup or any writable connection: a newer journal and its artifacts stay as found.
    if (QString format;
        journal_format::check(path, &format) == journal_format::Status::Unsupported) {
        result.outcome = Migration::Deferred;
        result.error = format;
        return result;
    }
    // An earlier attempt stopped before its rename: the original is the journal.
    if (!removeCompactCopy(path)) {
        result.outcome = Migration::Failed;
        result.error = QStringLiteral("An unsupported or busy migration artifact was preserved.");
        return result;
    }
    if (!QFileInfo::exists(path))
        return result;

    QString error;
    // Exclusive from its first read to its close: SQLite refuses it while any other connection
    // has the journal open, a daemon from before the journal lock included, and refuses every
    // other connection while it is open. The copy and the rename happen under it.
    Connection source(path, QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
    if (!source.database().isOpen())
        return failed(result, path, source.database().lastError().text());
    QSqlQuery first(source.database());
    if (!first.exec(QStringLiteral("PRAGMA locking_mode=EXCLUSIVE")))
        return failed(result, path, first.lastError().text());
    if (!first.exec(QStringLiteral("SELECT count(*) FROM sqlite_schema")) || !first.next()) {
        const QString code = first.lastError().nativeErrorCode();
        // SQLITE_BUSY or SQLITE_LOCKED: open elsewhere. The file is not touched.
        if (code == QStringLiteral("5") || code == QStringLiteral("6")) {
            result.outcome = Migration::Deferred;
            result.error = QStringLiteral("The journal is open in another process.");
            return result;
        }
        return failed(result, path, first.lastError().text());
    }
    const qint64 tables = first.value(0).toLongLong();
    first.finish();
    // A newer daemon's journal is never rewritten; LocalJournal then refuses to open it.
    if (!first.exec(QStringLiteral("PRAGMA user_version")) || !first.next())
        return failed(result, path, first.lastError().text());
    if (const int version = first.value(0).toInt(); !journal_format::supported(version)) {
        result.outcome = Migration::Deferred;
        result.error = QStringLiteral("The journal has the unsupported format %1.").arg(version);
        return result;
    }
    first.finish();
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
    if (QFileInfo(path + QStringLiteral("-wal")).size() > 0)
        return failed(result, path, QStringLiteral("The journal's log was not emptied."));

    // SQLite creates the output itself; keep it and its sidecars private throughout writing.
    // Some platform SQLite builds reject even an existing empty VACUUM INTO target.
    if (!QDir().mkdir(compactDirectory(path), privateDirectory)) {
        result.outcome = Migration::Failed;
        result.error = QStringLiteral("The private migration directory could not be created.");
        return result;
    }
#if !defined(Q_OS_WIN)
    const QFileInfo createdDirectory(compactDirectory(path));
    if (createdDirectory.ownerId() != ::geteuid() ||
        (createdDirectory.permissions() & groupOrOther)) {
        QDir().rmdir(compactDirectory(path)); // Empty directory created above; no data was copied.
        result.outcome = Migration::Failed;
        result.error = QStringLiteral("The migration directory could not be protected.");
        return result;
    }
#endif

    // The copy takes the pending auto_vacuum; the original is not changed.
    QSqlQuery vacuum(source.database());
    if (!vacuum.exec(QStringLiteral("PRAGMA auto_vacuum = INCREMENTAL")) ||
        !vacuum.prepare(QStringLiteral("VACUUM INTO ?")))
        return failed(result, path, vacuum.lastError().text());
    vacuum.addBindValue(compactPath(path));
    if (!vacuum.exec())
        return failed(result, path, vacuum.lastError().text());
    vacuum.finish();

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
        if (copiedTables->toLongLong() != tables || after->autoVacuum != 2)
            return failed(result, path,
                          QStringLiteral("The compacted journal does not match the original."));
        result.after = *after;
    }
#if !defined(Q_OS_WIN)
    if (!QFile::setPermissions(compactPath(path), QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return failed(result, path,
                      QStringLiteral("The compacted journal could not be protected."));
#endif
    if (!syncFile(compactPath(path)))
        return failed(result, path, QStringLiteral("The compacted journal could not be synced."));
#if defined(Q_OS_WIN)
    // Windows cannot replace an open file; the journal lock still keeps new daemons out.
    source.close();
#endif
    if (!replaceFile(compactPath(path), path))
        return failed(result, path,
                      QStringLiteral("The compacted journal could not replace the original."));
    syncDirectory(QFileInfo(path).absolutePath());
    source.close();
    // The old log, emptied before the copy, would only be an empty file beside the new journal.
    if (QFileInfo(path + QStringLiteral("-wal")).size() == 0)
        QFile::remove(path + QStringLiteral("-wal"));
    result.outcome = Migration::Migrated;
    if (!removeCompactCopy(path))
        result.error =
            QStringLiteral("Migration succeeded; staging artifacts were preserved for repair.");
    return result;
}

} // namespace ketplus::daemon::journal_space
