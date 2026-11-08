#include "storage/JournalSpace.h"
#include "service/DaemonService.h"
#include "storage/JournalFormat.h"
#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

#if !defined(Q_OS_WIN)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ketplus::daemon {
namespace {

// A connection of the test's own, closed before the code under test opens the file.
class Database final {
  public:
    explicit Database(const QString& path)
        : name_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        database_.open();
    }
    ~Database() {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool exec(const QString& statement) {
        QSqlQuery query(database_);
        return query.exec(statement);
    }
    QSqlDatabase& database() { return database_; }
    QVariant value(const QString& statement) {
        QSqlQuery query(database_);
        return query.exec(statement) && query.next() ? query.value(0) : QVariant();
    }

  private:
    QString name_;
    QSqlDatabase database_;
};

// 2,000 rows of 4 KiB, three quarters of them deleted: about 6 MB of free pages, as a journal
// that has pruned for weeks without returning them.
void writeJournalWithFreePages(const QString& path) {
    Database database(path);
    QVERIFY(database.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
    QVERIFY(
        database.exec(QStringLiteral("CREATE TABLE blobs (id INTEGER PRIMARY KEY, body BLOB)")));
    QVERIFY(database.exec(QStringLiteral("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 "
                                         "FROM n WHERE i < 2000) INSERT INTO blobs SELECT i, "
                                         "randomblob(4096) FROM n")));
    QVERIFY(database.exec(QStringLiteral("DELETE FROM blobs WHERE id % 4 != 0")));
}

} // namespace

class JournalSpaceTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void diskMeasurementsIncludeSidecarsWithoutChangingTheJournal() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        const auto length = [](const QString& file) {
            return QFileInfo::exists(file) ? QFileInfo(file).size() : qint64(0);
        };
        const qint64 mainBytes = length(path);
        const qint64 walBytes = length(path + "-wal");
        const qint64 shmBytes = length(path + "-shm");
        const auto usage = journal.diskUsage();
        QVERIFY(usage);
        QCOMPARE(usage->value("database_file_bytes").toInteger(), mainBytes);
        QCOMPARE(usage->value("wal_file_bytes").toInteger(), walBytes);
        QCOMPARE(usage->value("shared_memory_file_bytes").toInteger(), shmBytes);
        QCOMPARE(usage->value("total_file_bytes").toInteger(), mainBytes + walBytes + shmBytes);
        QVERIFY(mainBytes > 0);
        QCOMPARE(length(path), mainBytes);
        QCOMPARE(length(path + "-wal"), walBytes);
        QCOMPARE(length(path + "-shm"), shmBytes);
        QVERIFY(QDateTime::fromString(usage->value("sampled_at").toString(), Qt::ISODateWithMs)
                    .isValid());
        QVERIFY(usage->value("filesystem_available_bytes").toInteger(-1) >= 0);
        QVERIFY(!QJsonDocument(*usage).toJson().contains(directory.path().toUtf8()));
        LocalJournal memory(":memory:");
        QVERIFY(!memory.diskUsage());
    }
    void migratesNineGiBAndChecksRetainedBytes() {
        if (qEnvironmentVariable("KETPLUS_JOURNAL_SCALE_9GIB") != "1")
            QSKIP("Opt in with benchmarks/run-journal-scale.sh; needs 24 GiB free disk.");
        constexpr qint64 gib = 1024LL * 1024 * 1024;
        const QString root = QCoreApplication::applicationDirPath();
        QVERIFY2(QStorageInfo(root).bytesAvailable() >= 24 * gib,
                 "The isolated 9 GiB fixture requires at least 24 GiB free before starting.");
        QTemporaryDir directory(root + "/journal-scale-XXXXXX");
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("journal.sqlite3");
        const auto fingerprint = [](Database& database) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QSqlQuery query(database.database());
            query.setForwardOnly(true);
            if (!query.exec("SELECT id,body FROM blobs ORDER BY id"))
                return QByteArray{};
            while (query.next()) {
                hash.addData(QByteArray::number(query.value(0).toLongLong()));
                hash.addData(QByteArray(1, '\0'));
                hash.addData(query.value(1).toByteArray());
            }
            return query.lastError().isValid() ? QByteArray{} : hash.result();
        };
        QByteArray beforeHash;
        QElapsedTimer timer;
        timer.start();
        {
            Database database(path);
            QVERIFY(database.exec("PRAGMA journal_mode=DELETE"));
            QVERIFY(
                database.exec("PRAGMA synchronous=OFF")); // disposable fixture construction only
            QVERIFY(database.exec("CREATE TABLE blobs(id INTEGER PRIMARY KEY,body BLOB)"));
            QVERIFY(database.exec("BEGIN"));
            for (int batch = 0; batch < 9; ++batch) {
                QVERIFY2(QStorageInfo(root).bytesAvailable() >= 8 * gib,
                         "Stopped fixture creation to preserve disk headroom.");
                QVERIFY(database.exec(
                    QString("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n WHERE "
                            "i<1024) INSERT INTO blobs SELECT i+%1,randomblob(1048576) FROM n")
                        .arg(batch * 1024)));
                qInfo() << "Seeded" << batch + 1 << "GiB of isolated random payloads";
            }
            QVERIFY(database.exec("DELETE FROM blobs WHERE id>3584"));
            QVERIFY(database.exec("COMMIT"));
            QCOMPARE(database.value("PRAGMA auto_vacuum").toInt(), 0);
            beforeHash = fingerprint(database);
            QVERIFY(!beforeHash.isEmpty());
            QCOMPARE(database.value("SELECT count(*) FROM blobs").toInt(), 3584);
        }
        const qint64 before = QFileInfo(path).size();
        QVERIFY(before >= 9'000'000'000LL);
        qInfo() << "Fixture bytes:" << before << "construction/hash ms:" << timer.elapsed();
        timer.restart();
        const auto result = journal_space::migrateToIncrementalVacuum(path);
        const auto migrationMs = timer.elapsed();
        QVERIFY2(result.outcome == journal_space::Migration::Migrated, qPrintable(result.error));
        QCOMPARE(result.after.autoVacuum, 2);
        QCOMPARE(result.after.freePages, 0);
        const qint64 after = QFileInfo(path).size();
        QVERIFY(after < 4'000'000'000LL);
        {
            Database database(path);
            QCOMPARE(database.value("SELECT count(*) FROM blobs").toInt(), 3584);
            QCOMPARE(fingerprint(database), beforeHash);
            QCOMPARE(database.value("PRAGMA integrity_check").toString(), QString("ok"));
        }
        QVERIFY(!QFileInfo::exists(path + ".compact"));
        QCOMPARE(journal_space::migrateToIncrementalVacuum(path).outcome,
                 journal_space::Migration::NotNeeded);
        qInfo() << "Migrated bytes:" << before << "->" << after << "migration ms:" << migrationMs
                << "retained SHA-256:" << beforeHash.toHex();
        QVERIFY(directory.remove());
        QVERIFY(!QFileInfo::exists(path));
    }

    void migrationCopyIsPrivateWhileBeingWritten_data() {
        QTest::addColumn<quint32>("mask");
        QTest::newRow("usual-umask") << quint32(0022);
        QTest::newRow("private-umask-control") << quint32(0077);
    }
    void migrationCopyIsPrivateWhileBeingWritten() {
#if defined(Q_OS_WIN)
        QSKIP("POSIX mode-bit observation; Windows ACL validation is separate.");
#else
        QFETCH(quint32, mask);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("journal.sqlite");
        const auto checksum = [&](Database& database) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QSqlQuery query(database.database());
            if (!query.exec("SELECT id,body FROM blobs ORDER BY id"))
                return QByteArray{};
            while (query.next()) {
                hash.addData(QByteArray::number(query.value(0).toLongLong()));
                hash.addData(query.value(1).toByteArray());
            }
            return query.lastError().isValid() ? QByteArray{} : hash.result();
        };
        QByteArray before;
        {
            Database database(path);
            QVERIFY(database.exec("CREATE TABLE blobs(id INTEGER PRIMARY KEY, body BLOB)"));
            // Keep the copy observable while SQLite writes it, without a user journal or GB
            // fixture.
            QVERIFY(
                database.exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM n "
                              "WHERE i<64) INSERT INTO blobs SELECT i,randomblob(1048576) FROM n"));
            before = checksum(database);
            QCOMPARE(before.size(), 32);
        }
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const mode_t previousMask = ::umask(mode_t(mask));
        const auto restoreMask = qScopeGuard([&] { ::umask(previousMask); });
        auto migration = std::async(
            std::launch::async, [&] { return journal_space::migrateToIncrementalVacuum(path); });
        int mainSamples = 0;
        QString exposed;
        do {
            for (const auto& base : {path + ".compact", path + ".compact-private/journal.sqlite"}) {
                for (const auto* suffix : {"", "-journal", "-wal", "-shm"}) {
                    struct stat info{};
                    if (::stat(QFile::encodeName(base + suffix).constData(), &info) != 0 ||
                        info.st_size == 0)
                        continue;
                    if (QString::fromLatin1(suffix).isEmpty())
                        ++mainSamples;
                    bool protectedContainer = false;
                    if (base.endsWith(".compact-private/journal.sqlite")) {
                        struct stat parent{};
                        protectedContainer =
                            ::lstat(QFile::encodeName(path + ".compact-private").constData(),
                                    &parent) == 0 &&
                            S_ISDIR(parent.st_mode) && (parent.st_mode & 0077) == 0 &&
                            parent.st_uid == ::geteuid();
                    }
                    if ((info.st_mode & 0077) != 0 && !protectedContainer)
                        exposed = QString("copy mode %1 without private staging")
                                      .arg(info.st_mode & 0777, 3, 8, QLatin1Char('0'));
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (migration.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
        const auto result = migration.get();
        QVERIFY2(result.outcome == journal_space::Migration::Migrated, qPrintable(result.error));
        QVERIFY2(mainSamples > 0, "The observer must see a nonempty in-progress copy.");
        qInfo() << "Nonempty in-progress copy observations:" << mainSamples
                << "exposure:" << exposed;
        QVERIFY2(exposed.isEmpty(), qPrintable(exposed));
        QCOMPARE(QFile::permissions(path) &
                     (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                      QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther),
                 QFileDevice::Permissions());
        Database migrated(path);
        QCOMPARE(checksum(migrated), before);
        QCOMPARE(migrated.value("PRAGMA quick_check").toString(), QString("ok"));
        QCOMPARE(migrated.value("PRAGMA auto_vacuum").toInt(), 2);
        QVERIFY(!QFileInfo::exists(path + ".compact"));
        QVERIFY(!QFileInfo::exists(path + ".compact-private"));
#endif
    }

    void stalePrivateStagingIsBoundedAndPreservesUnsupportedEntries_data() {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"known", "unknown-entry", "symlink-entry", "symlink-root",
                                 "public-root", "file-root"})
            QTest::newRow(kind) << QString(kind);
    }
    void stalePrivateStagingIsBoundedAndPreservesUnsupportedEntries() {
#if defined(Q_OS_WIN)
        QSKIP("POSIX symlink/mode fixtures; Windows ACL checks remain separate.");
#else
        QFETCH(QString, kind);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("journal.sqlite");
        writeJournalWithFreePages(path);
        const auto stage = path + ".compact-private";
        const auto target = directory.filePath("preserved");
        QFile outside(target);
        QVERIFY(outside.open(QIODevice::WriteOnly));
        QCOMPARE(outside.write("keep"), qint64(4));
        outside.close();
        if (kind == "symlink-root") {
            const auto folder = directory.filePath("outside");
            QVERIFY(QDir().mkdir(folder));
            QVERIFY(QFile::link(folder, stage));
        } else if (kind == "file-root") {
            QVERIFY(QFile::copy(target, stage));
        } else {
            QVERIFY(QDir().mkdir(stage, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner));
            QVERIFY(QFile::copy(target, stage + "/journal.sqlite"));
            if (kind == "unknown-entry")
                QVERIFY(QFile::copy(target, stage + "/unknown"));
            else if (kind == "symlink-entry")
                QVERIFY(QFile::link(target, stage + "/journal.sqlite-wal"));
            else if (kind == "public-root")
                QVERIFY(QFile::setPermissions(stage, QFile::permissions(stage) |
                                                         QFileDevice::ReadOther |
                                                         QFileDevice::ExeOther));
        }
        const auto result = journal_space::migrateToIncrementalVacuum(path);
        if (kind == "known") {
            QVERIFY2(result.outcome == journal_space::Migration::Migrated,
                     qPrintable(result.error));
            QVERIFY(!QFileInfo::exists(stage));
        } else {
            QCOMPARE(result.outcome, journal_space::Migration::Failed);
            QVERIFY(!result.error.isEmpty());
            QVERIFY(QFileInfo::exists(stage) || QFileInfo(stage).isSymLink());
            if (kind != "symlink-root" && kind != "file-root") {
                QFile preserved(stage + "/journal.sqlite");
                QVERIFY(preserved.open(QIODevice::ReadOnly));
                QCOMPARE(preserved.readAll(), QByteArray("keep"));
            }
        }
        QVERIFY(outside.open(QIODevice::ReadOnly));
        QCOMPARE(outside.readAll(), QByteArray("keep"));
        Database original(path);
        QCOMPARE(original.value("SELECT count(*) FROM blobs").toInt(), 500);
        QCOMPARE(original.value("PRAGMA quick_check").toString(), QString("ok"));
        QCOMPARE(original.value("PRAGMA auto_vacuum").toInt(), kind == "known" ? 2 : 0);
#endif
    }

    void unavailableCopyDestinationPreservesJournalAndCanRetry() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("journal.sqlite");
        writeJournalWithFreePages(path);
        qlonglong before = 0;
        {
            Database database(path);
            before = database.value("SELECT sum(id)+sum(length(body)) FROM blobs").toLongLong();
        }
        const auto occupied = path + ".compact";
        QVERIFY(QDir().mkdir(occupied));
        QFile sentinel(occupied + "/keep");
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("keep"), qint64(4));
        sentinel.close();
        const auto result = journal_space::migrateToIncrementalVacuum(path);
        QCOMPARE(result.outcome, journal_space::Migration::Failed);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("keep"));
        sentinel.close();
        {
            Database database(path);
            QCOMPARE(database.value("SELECT sum(id)+sum(length(body)) FROM blobs").toLongLong(),
                     before);
            QCOMPARE(database.value("PRAGMA auto_vacuum").toInt(), 0);
        }
        QVERIFY(sentinel.remove());
        QVERIFY(QDir().rmdir(occupied));
        const auto retry = journal_space::migrateToIncrementalVacuum(path);
        QVERIFY2(retry.outcome == journal_space::Migration::Migrated, qPrintable(retry.error));
        Database migrated(path);
        QCOMPARE(migrated.value("SELECT sum(id)+sum(length(body)) FROM blobs").toLongLong(),
                 before);
    }

    void migratesAJournalWithFreePagesAndKeepsEveryRow() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        writeJournalWithFreePages(path);
        qlonglong checksum = 0;
        {
            Database database(path);
            checksum = database
                           .value(QStringLiteral("SELECT sum(id) + sum(length(body)) FROM "
                                                 "blobs"))
                           .toLongLong();
        }
        const qint64 sizeBefore = QFileInfo(path).size();

#if !defined(Q_OS_WIN)
        // The usual umask: a file SQLite creates is readable by others unless the copy says not.
        const mode_t umask = ::umask(022);
#endif
        const auto result = journal_space::migrateToIncrementalVacuum(path);
#if !defined(Q_OS_WIN)
        ::umask(umask);
#endif
        QVERIFY2(result.outcome == journal_space::Migration::Migrated, qPrintable(result.error));
        QCOMPARE(result.before.autoVacuum, 0);
        QVERIFY(result.before.freePages > 1'000);
        QCOMPARE(result.after.autoVacuum, 2);
        QCOMPARE(result.after.freePages, 0);
        QVERIFY(QFileInfo(path).size() < sizeBefore / 2);
        for (const QString& suffix : {QStringLiteral(".compact"),
                                      QStringLiteral(".compact-journal"), QStringLiteral("-wal")})
            QVERIFY2(!QFileInfo::exists(path + suffix), qPrintable(suffix));
#if !defined(Q_OS_WIN)
        QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions());
#endif
        {
            Database database(path);
            QCOMPARE(database.value(QStringLiteral("SELECT count(*) FROM blobs")).toInt(), 500);
            QCOMPARE(database.value(QStringLiteral("SELECT sum(id) + sum(length(body)) FROM blobs"))
                         .toLongLong(),
                     checksum);
            QCOMPARE(database.value(QStringLiteral("PRAGMA auto_vacuum")).toInt(), 2);
        }

        // Once on incremental vacuum, a journal is not copied again.
        QCOMPARE(journal_space::migrateToIncrementalVacuum(path).outcome,
                 journal_space::Migration::NotNeeded);
    }

    void aCopyLeftByAnInterruptedRunIsDroppedAndTheOriginalKept() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        writeJournalWithFreePages(path);
        // Stopped mid-copy last time: a partial file beside the journal.
        QFile partial(path + QStringLiteral(".compact"));
        QVERIFY(partial.open(QIODevice::WriteOnly));
        partial.write(QByteArray(8192, 'x'));
        partial.close();

        const auto result = journal_space::migrateToIncrementalVacuum(path);
        QVERIFY2(result.outcome == journal_space::Migration::Migrated, qPrintable(result.error));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".compact")));
        Database database(path);
        QCOMPARE(database.value(QStringLiteral("SELECT count(*) FROM blobs")).toInt(), 500);
    }

    void aJournalThatCannotBeReadIsLeftAsItIs() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes(16384, 'k');
        file.write(bytes);
        file.close();

        const auto result = journal_space::migrateToIncrementalVacuum(path);
        QCOMPARE(result.outcome, journal_space::Migration::Failed);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".compact")));
        QFile kept(path);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), bytes);
    }

    void aJournalOpenInAnOlderDaemonIsNotReplaced() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        writeJournalWithFreePages(path);
        // A daemon from before the journal lock: it owns only its endpoint lock, and keeps its
        // connection open while idle.
        Database older(path);
        QVERIFY(older.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
        QCOMPARE(older.value(QStringLiteral("SELECT count(*) FROM blobs")).toInt(), 500);
        const qint64 sizeBefore = QFileInfo(path).size();

        const auto result = journal_space::migrateToIncrementalVacuum(path);
        QCOMPARE(result.outcome, journal_space::Migration::Deferred);
        QCOMPARE(result.error, QStringLiteral("The journal is open in another process."));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".compact")));
        QCOMPARE(QFileInfo(path).size(), sizeBefore);
        // Its journal still works.
        QVERIFY(older.exec(QStringLiteral("INSERT INTO blobs VALUES (100000, x'00')")));
        QCOMPARE(older.value(QStringLiteral("SELECT count(*) FROM blobs")).toInt(), 501);
        QCOMPARE(older.value(QStringLiteral("PRAGMA auto_vacuum")).toInt(), 0);
    }

    void aNewerJournalIsRefusedWithoutAnyWrite() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        writeJournalWithFreePages(path);
        {
            // A later KetPlus wrote this journal; settle it into one file to compare its bytes.
            Database newer(path);
            QVERIFY(newer.exec(
                QStringLiteral("PRAGMA user_version=%1").arg(journal_format::current + 1)));
            QVERIFY(newer.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")));
            QVERIFY(newer.exec(QStringLiteral("PRAGMA journal_mode=DELETE")));
        }
        const auto bytes = [&path] {
            QFile file(path);
            return file.open(QIODevice::ReadOnly)
                       ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
                       : QByteArray();
        };
        const QByteArray before = bytes();
        QVERIFY(!before.isEmpty());

        // Its auto_vacuum would otherwise make startup compaction rewrite it.
        const auto result = journal_space::migrateToIncrementalVacuum(path);
        QCOMPARE(result.outcome, journal_space::Migration::Deferred);
        QCOMPARE(result.error, QStringLiteral("The journal has the newer format %1.")
                                   .arg(journal_format::current + 1));
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".compact")));
        {
            LocalJournal journal(path);
            QVERIFY(!journal.isOpen());
            QVERIFY2(journal.errorString().contains(QStringLiteral("newer KetPlus")),
                     qPrintable(journal.errorString()));
        }
        QCOMPARE(bytes(), before);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral("-wal")) ||
                QFileInfo(path + QStringLiteral("-wal")).size() == 0);
        Database check(path);
        QCOMPARE(check.value(QStringLiteral("PRAGMA user_version")).toInt(),
                 journal_format::current + 1);
        QCOMPARE(check.value(QStringLiteral("SELECT count(*) FROM blobs")).toInt(), 500);
    }

    void journalsRecordTheirFormatOnceUpgraded() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        {
            LocalJournal journal(path);
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        }
        {
            Database written(path);
            QCOMPARE(written.value(QStringLiteral("PRAGMA user_version")).toInt(),
                     journal_format::current);
            // A journal from before versioning reads 0.
            QVERIFY(written.exec(QStringLiteral("PRAGMA user_version=0")));
        }
        {
            LocalJournal legacy(path);
            QVERIFY2(legacy.isOpen(), qPrintable(legacy.errorString()));
        }
        Database upgraded(path);
        QCOMPARE(upgraded.value(QStringLiteral("PRAGMA user_version")).toInt(),
                 journal_format::current);
    }

    void aNewJournalNeedsNoCopyAndStartsOnIncrementalVacuum() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        QCOMPARE(journal_space::migrateToIncrementalVacuum(path).outcome,
                 journal_space::Migration::NotNeeded);
        QVERIFY(!QFileInfo::exists(path));
        LocalJournal journal(path);
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const auto space = journal.space();
        QVERIFY(space);
        QCOMPARE(space->autoVacuum, 2);
    }

    void freePagesGoBackToTheDiskInBatches() {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        writeJournalWithFreePages(path);
        {
            // Not yet migrated: nothing is moved.
            LocalJournal journal(path);
            QVERIFY(journal.isOpen());
            QCOMPARE(journal.reclaimFreePages(100'000), std::optional<qint64>(0));
        }
        QCOMPARE(journal_space::migrateToIncrementalVacuum(path).outcome,
                 journal_space::Migration::Migrated);
        {
            Database database(path);
            QVERIFY(database.exec(QStringLiteral("DELETE FROM blobs WHERE id % 100 != 0")));
        }
        LocalJournal journal(path);
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const qint64 freed = journal.space()->freePages;
        QVERIFY2(freed > LocalJournal::minimumReclaimPages + 100,
                 qPrintable(QString::number(freed)));
        const qint64 pagesBefore = journal.space()->pageCount;

        const qint64 pageSize = journal.space()->pageSize;
        const auto fileShrunkTo = [&](const qint64 pages) {
            return QFileInfo(path).size() == pages * pageSize &&
                   QFileInfo(path + QStringLiteral("-wal")).size() == 0;
        };

        // The disk gets the pages back while the journal stays open.
        QCOMPARE(journal.reclaimFreePages(100), std::optional<qint64>(100));
        QCOMPARE(journal.space()->freePages, freed - 100);
        QCOMPARE(journal.space()->pageCount, pagesBefore - 100);
        QVERIFY2(fileShrunkTo(pagesBefore - 100),
                 qPrintable(QString::number(QFileInfo(path).size())));

        // A reader holds the checkpoint back. The batch leaves fewer free pages than the minimum,
        // so the next call has nothing to reclaim and only finishes the checkpoint.
        const qint64 batch = journal.space()->freePages - 100;
        QVERIFY(100 < LocalJournal::minimumReclaimPages);
        const qint64 shrunk = pagesBefore - 100 - batch;
        const auto within = [](QElapsedTimer& elapsed) {
            // The daemon's thread never waits for a reader.
            return elapsed.elapsed() < 1'000;
        };
        auto older = std::make_unique<Database>(path);
        QVERIFY(older->database().transaction());
        QVERIFY(older->value(QStringLiteral("SELECT count(*) FROM blobs")).isValid());
        QElapsedTimer elapsed;
        elapsed.start();
        QCOMPARE(journal.reclaimFreePages(batch), std::optional<qint64>(batch));
        QVERIFY2(within(elapsed), qPrintable(QString::number(elapsed.elapsed())));
        QVERIFY(QFileInfo(path).size() > shrunk * pageSize);

        // The next reader starts on the newest snapshot, then the first one ends: the log can be
        // copied back but not emptied while the second one reads it.
        Database newer(path);
        QVERIFY(newer.database().transaction());
        QVERIFY(newer.value(QStringLiteral("SELECT count(*) FROM blobs")).isValid());
        older->database().commit();
        older.reset();
        elapsed.restart();
        QCOMPARE(journal.reclaimFreePages(100'000), std::optional<qint64>(0));
        QVERIFY2(within(elapsed), qPrintable(QString::number(elapsed.elapsed())));
        QCOMPARE(QFileInfo(path).size(), shrunk * pageSize);
        newer.database().commit();
        QCOMPARE(journal.reclaimFreePages(100'000), std::optional<qint64>(0));
        QCOMPARE(QFileInfo(path).size(), shrunk * pageSize);
        // Below the minimum a batch is not worth a write.
        QCOMPARE(journal.space()->freePages, 100);
        QCOMPARE(journal.reclaimFreePages(100'000), std::optional<qint64>(0));
        QCOMPARE(journal.space()->freePages, 100);
    }

    void aSecondDaemonCannotOpenTheSameJournal() {
        QTemporaryDir directory;
        const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
        DaemonService first({state,
                             directory.filePath(QStringLiteral("first.token")),
                             directory.filePath(QStringLiteral("first.sock")),
                             {},
                             {},
                             {}});
        QVERIFY2(first.start(), qPrintable(first.errorString()));
        DaemonService second({state,
                              directory.filePath(QStringLiteral("second.token")),
                              directory.filePath(QStringLiteral("second.sock")),
                              {},
                              {},
                              {}});
        QVERIFY(!second.start());
        QCOMPARE(second.errorString(), QStringLiteral("Another KetPlus Daemon uses this journal."));
    }
};

} // namespace ketplus::daemon

QTEST_GUILESS_MAIN(ketplus::daemon::JournalSpaceTest)
#include "JournalSpaceTest.moc"
