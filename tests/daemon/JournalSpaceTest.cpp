#include "storage/JournalSpace.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QVariant>

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
        for (const QString& suffix :
             {QStringLiteral(".compact"), QStringLiteral(".compact-journal"),
              QStringLiteral("-wal"), QStringLiteral("-shm")})
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
            QVERIFY(database.exec(QStringLiteral("DELETE FROM blobs WHERE id % 40 != 0")));
        }
        LocalJournal journal(path);
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const qint64 freed = journal.space()->freePages;
        QVERIFY2(freed > LocalJournal::minimumReclaimPages + 100,
                 qPrintable(QString::number(freed)));
        const qint64 pagesBefore = journal.space()->pageCount;

        QCOMPARE(journal.reclaimFreePages(100), std::optional<qint64>(100));
        QCOMPARE(journal.space()->freePages, freed - 100);
        QCOMPARE(journal.space()->pageCount, pagesBefore - 100);
        QCOMPARE(journal.reclaimFreePages(freed - 200), std::optional<qint64>(freed - 200));
        QCOMPARE(journal.space()->freePages, 100);
        // Below the minimum a batch is not worth a write.
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
