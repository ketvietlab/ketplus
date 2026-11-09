#include "storage/JournalFormat.h"
#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

// Opening a journal from before the format marker migrates it in place: columns, indexes,
// backfills, activity markers and finally the format. These tests stop that migration with a
// full disk and by killing the process, then check the journal opens to the same result.
namespace {
constexpr auto childFlag = "--open-journal";

QSqlDatabase journalConnection(const QString& path) {
    for (const auto& name : QSqlDatabase::connectionNames())
        if (name.startsWith("ketplus-daemon-") &&
            QSqlDatabase::database(name).databaseName() == path)
            return QSqlDatabase::database(name);
    return {};
}

// A journal written before the format marker, the activity index, the turn indexes and several
// columns existed. Each session numbers its own cursors from 1, as a real journal does, so the
// turn-number backfill applies to every session.
bool writeLegacyJournal(const QString& path, int rows, int sessions) {
    LocalJournal journal(path);
    if (!journal.isOpen())
        return false;
    QSqlDatabase db = journalConnection(path);
    QSqlQuery q(db);
    const auto n = [](int count) {
        return QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE "
                       "i<%1) ")
            .arg(count);
    };
    const QString session = QString("'s'||(1+i%%1)").arg(sessions);
    const QString cursor = QString("(1+(i-1)/%1)").arg(sessions);
    // Every tenth event of a session starts a turn, so the turn backfills have work.
    const QString type =
        "CASE WHEN " + cursor + "%10=1 THEN 'user.message' ELSE 'item.started' END";
    const QString payload = "CASE WHEN " + cursor +
                            "%10=1 THEN json_object('text','prompt '||i) ELSE "
                            "json_object('item_id','t'||i,'item',json_object('id','t'||i,'type',"
                            "'commandExecution','command',printf('%.120c','x'))) END";
    const QString sentAt = "strftime('%Y-%m-%dT%H:%M:%fZ','2026-01-01',printf('+%d seconds',i))";
    const QStringList statements{
        "BEGIN",
        n(sessions) +
            "INSERT INTO sessions(id,worktree_path,adapter,thread_id,turn_id,state,"
            "generation,last_cursor) SELECT 's'||i,'/tmp/w'||(i%7),'codex','','','ready',1,0 "
            "FROM n",
        n(rows) +
            "INSERT INTO events(session_id,generation,cursor,event_type,payload,sent_at) "
            "SELECT " +
            session + ",1," + cursor + "," + type + "," + payload + "," + sentAt + " FROM n",
        n(rows) +
            "INSERT INTO replay_events(session_id,generation,cursor,event_type,item_id,"
            "payload,sent_at) SELECT " +
            session + ",1," + cursor + "," + type + ",'t'||i," + payload + "," + sentAt + " FROM n",
        "UPDATE sessions SET last_cursor=(SELECT MAX(cursor) FROM events e WHERE "
        "e.session_id=sessions.id)",
        "INSERT INTO replay_projection_state(session_id,generation,first_cursor,last_cursor) "
        "SELECT id,1,1,last_cursor FROM sessions",
        "DROP INDEX events_turn",
        "DROP INDEX replay_events_turn",
        "DROP INDEX replay_events_item_cursor",
        "DROP INDEX replay_events_item_transfer",
        "DROP TABLE activity_items",
        "DROP TABLE activity_summaries",
        "DROP TABLE activity_heads",
        "DROP TABLE activity_backfill",
        "DELETE FROM worktree_catalog",
        "COMMIT",
        // Columns later releases added; the migration adds them back.
        "ALTER TABLE sessions DROP COLUMN last_turn_started_at",
        "ALTER TABLE sessions DROP COLUMN last_seen_cursor",
        "ALTER TABLE replay_projection_state DROP COLUMN revision",
        "ALTER TABLE worktree_catalog DROP COLUMN repository_path",
        "ALTER TABLE cloud_events DROP COLUMN upload_acked",
        "PRAGMA user_version=0",
        "PRAGMA wal_checkpoint(TRUNCATE)",
    };
    for (const auto& statement : statements)
        if (!q.exec(statement)) {
            qWarning() << statement.left(80) << q.lastError().text();
            return false;
        }
    return true;
}

// The parts of a journal its migration decides, independent of the clock.
QVariantMap contents(const QString& path) {
    const QString name = QUuid::createUuid().toString();
    QVariantMap result;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (!db.open())
            return {};
        QSqlQuery q(db);
        q.exec("PRAGMA integrity_check");
        result.insert("integrity", q.next() ? q.value(0) : QVariant());
        q.exec("PRAGMA user_version");
        result.insert("format", q.next() ? q.value(0) : QVariant());
        // Definitions, not only names: an index of the same name on other columns differs.
        QStringList schema;
        if (!q.exec("SELECT type||' '||name||' '||COALESCE(sql,'') FROM sqlite_master ORDER BY 1"))
            schema.append("query failed: " + q.lastError().text());
        while (q.next())
            schema.append(q.value(0).toString());
        result.insert("schema", schema);
        const QList<QPair<QString, QString>> tables{
            {"sessions", "SELECT * FROM sessions ORDER BY 1"},
            {"events", "SELECT * FROM events ORDER BY 1,2,3"},
            {"replay_events", "SELECT * FROM replay_events ORDER BY 1,2,3"},
            {"replay_projection_state", "SELECT * FROM replay_projection_state ORDER BY 1,2"},
            {"activity_items", "SELECT * FROM activity_items ORDER BY 1,2,3,4"},
            {"activity_summaries", "SELECT * FROM activity_summaries ORDER BY 1,2,3,4"},
            {"activity_heads", "SELECT * FROM activity_heads ORDER BY 1,2"},
            {"activity_backfill", "SELECT * FROM activity_backfill ORDER BY 1,2"},
            // updated_at is the clock at migration time.
            {"worktree_catalog", "SELECT path,catalog_revision,lifecycle_revision,"
                                 "repository_path FROM worktree_catalog ORDER BY 1"},
        };
        for (const auto& [table, select] : tables) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            qint64 count = 0;
            if (!q.exec(select)) {
                result.insert(table, "query failed: " + q.lastError().text());
                continue;
            }
            while (q.next()) {
                ++count;
                for (int i = 0; i < q.record().count(); ++i)
                    hash.addData(q.value(i).toString().toUtf8() + '\x1f');
            }
            result.insert(table, QString("%1 %2").arg(count).arg(hash.result().toHex()));
        }
    }
    QSqlDatabase::removeDatabase(name);
    return result;
}

bool copyJournal(const QString& from, const QString& to) {
    QFile::remove(to);
    return QFile::copy(from, to);
}

int openInChild(const QString& path, int killAfterMs, bool* killed) {
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    child.start(QCoreApplication::applicationFilePath(), {childFlag, path});
    if (!child.waitForStarted())
        return -1;
    if (killAfterMs >= 0 && !child.waitForFinished(killAfterMs)) {
        child.kill();
        child.waitForFinished();
        *killed = true;
        return -1;
    }
    child.waitForFinished(-1);
    *killed = false;
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : -1;
}

class MountedImage {
  public:
    explicit MountedImage(const QString& dir) : image_(dir + "/disk.dmg"), mount_(dir + "/mnt") {
        if (QProcess::execute("hdiutil", {"create", "-size", "256m", "-fs", "HFS+", "-volname",
                                          "ketplus-full", "-layout", "NONE", image_, "-quiet"}) ==
            0)
            mounted_ = QProcess::execute("hdiutil", {"attach", image_, "-mountpoint", mount_,
                                                     "-nobrowse", "-quiet"}) == 0;
    }
    ~MountedImage() {
        if (mounted_)
            QProcess::execute("hdiutil", {"detach", mount_, "-force", "-quiet"});
    }
    [[nodiscard]] bool mounted() const { return mounted_; }
    [[nodiscard]] QString path(const QString& name) const { return mount_ + "/" + name; }

  private:
    QString image_;
    QString mount_;
    bool mounted_{false};
};
} // namespace

class JournalMigrationRecoveryTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QVERIFY(writeLegacyJournal(fixtures_.filePath("legacy.sqlite"), 60000, 2000));
        QVERIFY(copyJournal(fixtures_.filePath("legacy.sqlite"),
                            fixtures_.filePath("reference.sqlite")));
        QElapsedTimer timer;
        timer.start();
        bool killed = false;
        QCOMPARE(openInChild(fixtures_.filePath("reference.sqlite"), -1, &killed), 0);
        migrationMs_ = timer.elapsed();
        reference_ = contents(fixtures_.filePath("reference.sqlite"));
        QCOMPARE(reference_.value("integrity").toString(), QString("ok"));
        QCOMPARE(reference_.value("format").toInt(), journal_format::current);
        for (auto it = reference_.cbegin(); it != reference_.cend(); ++it)
            QVERIFY2(!it.value().toString().startsWith("query failed"), qPrintable(it.key()));
        QVERIFY(reference_ != contents(fixtures_.filePath("legacy.sqlite")));
        // Every session's legacy backfills ran, and the catalog learned its worktrees.
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "reference-check");
            db.setDatabaseName(fixtures_.filePath("reference.sqlite"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT COUNT(*) FROM sessions WHERE current_turn_sequence=0 OR "
                           "title='' OR last_turn_started_at=''") &&
                    q.next());
            QCOMPARE(q.value(0).toInt(), 0);
            QVERIFY(q.exec("SELECT COUNT(*) FROM worktree_catalog") && q.next());
            QCOMPARE(q.value(0).toInt(), 7);
        }
        QSqlDatabase::removeDatabase("reference-check");
        qInfo() << "uninterrupted migration in a child process:" << migrationMs_ << "ms";
    }

    void aFullDiskFailsTheOpenAndTheNextOpenFinishes() {
        if (QStandardPaths::findExecutable("hdiutil").isEmpty())
            QSKIP("Needs hdiutil to mount a small volume");
        QTemporaryDir dir;
        MountedImage volume(dir.path());
        if (!volume.mounted())
            QSKIP("Could not mount a disk image");
        const QString path = volume.path("journal.sqlite");
        QVERIFY(QFile::copy(fixtures_.filePath("legacy.sqlite"), path));
        const auto legacy = contents(path);
        // Leave the volume about 256 KiB short of what the indexes and backfills need.
        QFile filler(volume.path("filler"));
        QVERIFY(filler.open(QIODevice::WriteOnly));
        const QByteArray block(64 * 1024, '\0');
        QStorageInfo storage(volume.path(""));
        while (storage.refresh(), storage.bytesAvailable() > 256 * 1024)
            if (filler.write(block) != block.size())
                break;
        filler.close();
        {
            LocalJournal journal(path);
            QVERIFY(!journal.isOpen());
            QVERIFY2(journal.errorString().contains("full", Qt::CaseInsensitive),
                     qPrintable(journal.errorString()));
        }
        const auto failed = contents(path);
        QCOMPARE(failed.value("integrity").toString(), QString("ok"));
        // The format is stamped only after every step succeeded, so the next open repeats them.
        QCOMPARE(failed.value("format").toInt(), 0);
        QCOMPARE(failed.value("events"), legacy.value("events"));
        QCOMPARE(failed.value("replay_events"), legacy.value("replay_events"));
        QVERIFY(filler.remove());
        {
            LocalJournal journal(path);
            QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        }
        QCOMPARE(contents(path), reference_);
    }

    void aKilledMigrationFinishesOnTheNextOpen() {
        QTemporaryDir dir;
        const QString path = dir.filePath("journal.sqlite");
        const auto legacy = contents(fixtures_.filePath("legacy.sqlite"));
        int interrupted = 0;
        int midway = 0;
        for (int step = 1; step <= 6; ++step) {
            QVERIFY(copyJournal(fixtures_.filePath("legacy.sqlite"), path));
            QFile::remove(path + "-wal");
            QFile::remove(path + "-shm");
            const int delay = static_cast<int>(migrationMs_ * step / 7);
            bool killed = false;
            openInChild(path, delay, &killed);
            const auto stopped = contents(path);
            QCOMPARE(stopped.value("integrity").toString(), QString("ok"));
            if (killed && stopped.value("format").toInt() == 0) {
                ++interrupted;
                // Some steps committed, the rest had not run yet.
                if (stopped.value("schema") != legacy.value("schema"))
                    ++midway;
            }
            {
                LocalJournal journal(path);
                QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
            }
            QCOMPARE(contents(path), reference_);
        }
        qInfo() << "kills before the format was stamped:" << interrupted << "of 6, after a"
                << "committed step:" << midway;
        // The kill points must actually land inside the migration for this to show anything.
        QVERIFY(interrupted >= 2);
        QVERIFY(midway >= 1);
    }

  private:
    QTemporaryDir fixtures_;
    QVariantMap reference_;
    qint64 migrationMs_{0};
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && qstrcmp(argv[1], childFlag) == 0) {
        LocalJournal journal(QString::fromLocal8Bit(argv[2]));
        return journal.isOpen() ? 0 : 1;
    }
    JournalMigrationRecoveryTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "JournalMigrationRecoveryTest.moc"
