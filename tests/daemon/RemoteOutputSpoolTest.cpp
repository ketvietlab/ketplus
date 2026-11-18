// The output spool of a remote job on the task output store, alone and behind the real output
// intake, on SQLite files configured like the daemon's journal.
#include "runner/output/TaskOutputSpool.h"
#include "runner/peer/OutputIntake.h"
#include "storage/BackgroundTaskOutput.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

using ketplus::daemon::BackgroundTaskOutput;
using ketplus::daemon::runner::output::TaskOutputSpool;
using ketplus::daemon::runner::peer::OutputIntake;
using ketplus::daemon::runner::peer::OutputStep;

namespace {

constexpr const char* kTask = "rt_job_7";
constexpr const char* kSecret = "OUTPUT-CONTENT-MARKER";

QMutex logMutex;
QStringList logLines;
QtMessageHandler previousHandler = nullptr;

void recordLog(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    {
        QMutexLocker lock(&logMutex);
        logLines.append(message);
    }
    if (previousHandler)
        previousHandler(type, context, message);
}

// The spool's own lines since the last call; Qt's SQL warnings are left out.
QStringList takeLogs() {
    QMutexLocker lock(&logMutex);
    return std::exchange(logLines, {}).filter(QStringLiteral("output spool"));
}

// One connection to a store file, configured like the daemon's journal.
struct Store {
    explicit Store(const QString& path, const bool create = true, const bool readOnly = false)
        : name(QUuid::createUuid().toString()) {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (readOnly)
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (!db.open())
            return;
        configure();
        if (create) {
            QString error;
            BackgroundTaskOutput output(db);
            ready = output.initialize(&error);
        }
    }
    ~Store() {
        {
            auto db = QSqlDatabase::database(name, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    void configure() const {
        QSqlQuery query(db());
        query.exec("PRAGMA journal_mode=WAL");
        query.exec("PRAGMA synchronous=FULL");
    }
    [[nodiscard]] QSqlDatabase db() const { return QSqlDatabase::database(name, false); }
    [[nodiscard]] BackgroundTaskOutput output() const { return BackgroundTaskOutput(db()); }
    bool exec(const QString& sql) const {
        QSqlQuery query(db());
        return query.exec(sql);
    }
    // Every write to the output tables fails until heal().
    bool breakWrites() const {
        return exec("CREATE TRIGGER no_chunks BEFORE INSERT ON job_output_chunks "
                    "BEGIN SELECT RAISE(ABORT,'refused'); END") &&
               exec("CREATE TRIGGER no_heads BEFORE INSERT ON job_output_heads "
                    "BEGIN SELECT RAISE(ABORT,'refused'); END") &&
               exec("CREATE TRIGGER no_head_updates BEFORE UPDATE ON job_output_heads "
                    "BEGIN SELECT RAISE(ABORT,'refused'); END");
    }
    bool heal() const {
        return exec("DROP TRIGGER no_chunks") && exec("DROP TRIGGER no_heads") &&
               exec("DROP TRIGGER no_head_updates");
    }

    QString name;
    bool ready = false;
};

QByteArray readAll(const BackgroundTaskOutput& output, const QString& task) {
    QByteArray bytes;
    QString cursor;
    for (int n = 0; n < 4000; ++n) {
        const auto page = output.read(task, cursor);
        if (!page.error.isEmpty())
            return "error: " + page.error.toUtf8();
        bytes.append(page.bytes);
        if (page.bytes.isEmpty() && page.nextCursor == cursor)
            return bytes;
        cursor = page.nextCursor;
    }
    return "no end";
}

QByteArray pattern(const qsizetype size, const int seed = 0) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = char('A' + (i + seed) % 26);
    return bytes;
}

} // namespace

class RemoteOutputSpoolTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() { previousHandler = qInstallMessageHandler(recordLog); }
    void init() { takeLogs(); }

    void opensAtTheStoredOffset() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QVERIFY(store.ready);
        auto fresh = TaskOutputSpool::open(store.db(), kTask);
        QVERIFY(fresh);
        QCOMPARE(fresh->size(), 0);
        QString error;
        QVERIFY(store.output().append(kTask, "hello", &error));
        QVERIFY(store.output().markGap(kTask, 5, 9, &error));
        auto spool = TaskOutputSpool::open(store.db(), kTask);
        QVERIFY(spool);
        QCOMPARE(spool->size(), 9);
    }

    void cannotOpenWithoutAnOffset() {
        QTemporaryDir dir;
        Store bare(dir.filePath("bare.sqlite"), false);
        QVERIFY(!TaskOutputSpool::open(bare.db(), kTask));
        Store store(dir.filePath("o.sqlite"));
        QVERIFY(!TaskOutputSpool::open(store.db(), QString()));
        store.db().close();
        QVERIFY(!TaskOutputSpool::open(store.db(), kTask));
        const QStringList logs = takeLogs();
        QCOMPARE(logs.size(), 3);
        for (const QString& line : logs)
            QVERIFY2(line.contains("storage_error"), qPrintable(line));
    }

    void writesAndSkipsAtItsOwnSize() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        auto spool = TaskOutputSpool::open(store.db(), kTask);
        QVERIFY(spool->append("abc"));
        QVERIFY(spool->append({}));
        QCOMPARE(spool->size(), 3);
        QVERIFY(spool->markGap(3)); // equal: nothing to record
        QCOMPARE(spool->size(), 3);
        QVERIFY(!spool->markGap(2));
        QCOMPARE(spool->size(), 3);
        QVERIFY(spool->markGap(8));
        QCOMPARE(spool->size(), 8);
        QVERIFY(spool->append("de"));
        QCOMPARE(spool->size(), 10);
        QCOMPARE(store.output().total(kTask), std::optional<qint64>(10));
        QCOMPARE(readAll(store.output(), kTask), QByteArray("abcde"));
        QVERIFY(takeLogs().isEmpty());
    }

    void refusesWhenTheStoreMoved() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        Store store(path);
        Store other(path);
        auto spool = TaskOutputSpool::open(store.db(), kTask);
        QVERIFY(spool->append("abc"));
        QString error;
        QVERIFY(other.output().appendAt(kTask, 3, kSecret, &error));
        QVERIFY(!spool->append("def"));
        QVERIFY(!spool->markGap(100));
        QCOMPARE(spool->size(), 3);
        QCOMPARE(readAll(store.output(), kTask), QByteArray("abc") + kSecret);
        const QStringList logs = takeLogs();
        QCOMPARE(logs.size(), 2);
        for (const QString& line : logs) {
            QVERIFY2(line.contains(kTask) && line.contains("offset_mismatch"), qPrintable(line));
            QVERIFY(!line.contains(kSecret) && !line.contains("def"));
        }
    }

    void recoversAfterTheStoreFails() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        auto spool = TaskOutputSpool::open(store.db(), kTask);
        QVERIFY(spool->append("abc"));
        QVERIFY(store.breakWrites());
        QVERIFY(!spool->append(kSecret));
        QVERIFY(!spool->markGap(10));
        QCOMPARE(spool->size(), 3);
        QVERIFY(store.heal());
        QVERIFY(spool->append("def"));
        QCOMPARE(spool->size(), 6);

        // A closed connection fails the same way and works again once reopened.
        store.db().close();
        QVERIFY(!spool->append(kSecret));
        QVERIFY(!spool->markGap(10));
        QCOMPARE(spool->size(), 6);
        QVERIFY(store.db().open());
        store.configure();
        QVERIFY(spool->markGap(10));
        QVERIFY(spool->append("g"));
        QCOMPARE(spool->size(), 11);
        QCOMPARE(readAll(store.output(), kTask), QByteArray("abcdefg"));
        const QStringList logs = takeLogs();
        QCOMPARE(logs.size(), 4);
        for (const QString& line : logs) {
            QVERIFY2(line.contains(kTask) && line.contains("storage_error"), qPrintable(line));
            QVERIFY(!line.contains(kSecret) && !line.contains("refused"));
        }
    }

    void aReadOnlyStoreWritesNothing() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        Store store(path);
        QString error;
        QVERIFY(store.output().append(kTask, "abc", &error));
        Store reader(path, false, true);
        auto spool = TaskOutputSpool::open(reader.db(), kTask);
        QVERIFY(spool);
        QCOMPARE(spool->size(), 3);
        QVERIFY(!spool->append("def"));
        QVERIFY(!spool->markGap(9));
        QCOMPARE(spool->size(), 3);
        QCOMPARE(store.output().total(kTask), std::optional<qint64>(3));
        // The writable connection carries on from the same offset.
        auto writer = TaskOutputSpool::open(store.db(), kTask);
        QVERIFY(writer->append("def"));
        QCOMPARE(readAll(reader.output(), kTask), QByteArray("abcdef"));
    }

    void countsBytesBeyondTheLimit() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        const qint64 limit = BackgroundTaskOutput::maximumBytes;
        const QByteArray all = pattern(limit + 100'000);
        {
            Store store(path);
            auto spool = TaskOutputSpool::open(store.db(), kTask);
            for (qsizetype at = 0; at < all.size(); at += 512 * 1024)
                QVERIFY(spool->append(all.mid(at, 512 * 1024)));
            QCOMPARE(spool->size(), all.size());
        }
        Store reopened(path);
        auto spool = TaskOutputSpool::open(reopened.db(), kTask);
        QCOMPARE(spool->size(), all.size());
        QVERIFY(readAll(reopened.output(), kTask) == all.right(limit));
    }

    void reopensWhereItStopped() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        {
            Store store(path);
            auto spool = TaskOutputSpool::open(store.db(), kTask);
            QVERIFY(spool->append("abc"));
            QVERIFY(spool->markGap(7));
        }
        Store store(path);
        auto spool = TaskOutputSpool::open(store.db(), kTask);
        QCOMPARE(spool->size(), 7);
        QVERIFY(spool->append("xyz"));
        QCOMPARE(readAll(store.output(), kTask), QByteArray("abcxyz"));
        QCOMPARE(store.output().total(kTask), std::optional<qint64>(10));
    }

    void takesOutputThroughTheIntake() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        QByteArray expected;
        {
            Store store(path);
            auto spool = TaskOutputSpool::open(store.db(), kTask);
            OutputIntake intake(*spool);
            QCOMPARE(intake.page(0, "abc").kind, OutputStep::Kind::Written);
            QCOMPARE(intake.page(0, "abcdef").kind, OutputStep::Kind::Written); // overlaps
            QCOMPARE(intake.page(2, "cd").kind, OutputStep::Kind::Duplicate);
            const OutputStep ahead = intake.page(12, "late");
            QCOMPARE(ahead.kind, OutputStep::Kind::Ahead);
            QCOMPARE(ahead.resumeFrom, std::optional<qint64>(6));
            QCOMPARE(intake.gap(6, 12).kind, OutputStep::Kind::Written);
            QCOMPARE(intake.gap(6, 12).kind, OutputStep::Kind::Duplicate);
            QCOMPARE(intake.page(12, "late").kind, OutputStep::Kind::Written);
            expected = "abcdef"
                       "late";

            QVERIFY(store.breakWrites());
            QCOMPARE(intake.page(16, "more").kind, OutputStep::Kind::Failed);
            QCOMPARE(intake.gap(16, 20).kind, OutputStep::Kind::Failed);
            QCOMPARE(intake.size(), 16);
            QVERIFY(store.heal());
            QCOMPARE(intake.page(14, "temore").kind, OutputStep::Kind::Written); // resent
            expected += "more";
            QCOMPARE(intake.size(), 20);

            const QByteArray big = pattern(300 * 1024);
            const OutputStep step = intake.page(20, big);
            QCOMPARE(step.kind, OutputStep::Kind::Written);
            QCOMPARE(step.ackAt, std::optional<qint64>(20 + big.size()));
            expected += big;
            QVERIFY(readAll(store.output(), kTask) == expected);
        }
        Store reopened(path);
        auto spool = TaskOutputSpool::open(reopened.db(), kTask);
        OutputIntake intake(*spool);
        QCOMPARE(intake.size(), 20 + 300 * 1024);
        QCOMPARE(intake.page(0, "abc").kind, OutputStep::Kind::Duplicate);
        QVERIFY(readAll(reopened.output(), kTask) == expected);
    }

    // 10 MiB in 32 KiB pages through the spool, on a file with WAL and synchronous=FULL.
    void measuresPageWrites() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        auto spool = TaskOutputSpool::open(store.db(), kTask);
        const QByteArray page = pattern(32 * 1024);
        std::vector<qint64> micros;
        QElapsedTimer total;
        total.start();
        for (int n = 0; n < 320; ++n) {
            QElapsedTimer timer;
            timer.start();
            QVERIFY(spool->append(page));
            micros.push_back(timer.nsecsElapsed() / 1000);
        }
        const qint64 elapsed = total.elapsed();
        std::sort(micros.begin(), micros.end());
        qInfo("spool page writes: 320 x 32 KiB in %lld ms, median %.2f ms, p95 %.2f ms, "
              "max %.2f ms",
              elapsed, double(micros[160]) / 1000.0, double(micros[304]) / 1000.0,
              double(micros.back()) / 1000.0);
        QCOMPARE(spool->size(), 320 * 32 * 1024);
    }
};

QTEST_GUILESS_MAIN(RemoteOutputSpoolTest)
#include "RemoteOutputSpoolTest.moc"
