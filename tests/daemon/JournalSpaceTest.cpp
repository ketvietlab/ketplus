#include "storage/JournalSpace.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

#include <memory>

#if !defined(Q_OS_WIN)
#include <sys/stat.h>
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
