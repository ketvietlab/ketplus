// The task output store at expected offsets, with gaps, and pages read across them.
#include "storage/BackgroundTaskOutput.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

namespace {

constexpr qint64 kMax = BackgroundTaskOutput::maximumBytes;

// One connection to a store file, configured like the daemon's journal.
struct Store {
    explicit Store(const QString& path) : name(QUuid::createUuid().toString()) {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (!db.open())
            return;
        QSqlQuery query(db);
        query.exec("PRAGMA journal_mode=WAL");
        query.exec("PRAGMA synchronous=FULL");
        output = std::make_unique<BackgroundTaskOutput>(db);
        QString error;
        ready = output->initialize(&error);
    }
    ~Store() {
        output.reset();
        {
            auto db = QSqlDatabase::database(name, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    [[nodiscard]] QSqlDatabase db() const { return QSqlDatabase::database(name, false); }
    // (position, size) of each stored chunk of `task`, in order.
    [[nodiscard]] QList<std::pair<qint64, qint64>> chunks(const QString& task) const {
        QList<std::pair<qint64, qint64>> list;
        QSqlQuery query(db());
        query.prepare("SELECT position,length(bytes) FROM job_output_chunks WHERE task_id=? "
                      "ORDER BY position");
        query.addBindValue(task);
        if (query.exec())
            while (query.next())
                list.append({query.value(0).toLongLong(), query.value(1).toLongLong()});
        return list;
    }
    [[nodiscard]] qint64 stored(const QString& task) const {
        qint64 sum = 0;
        for (const auto& [position, size] : chunks(task))
            sum += size;
        return sum;
    }

    QString name;
    std::unique_ptr<BackgroundTaskOutput> output;
    bool ready = false;
};

struct Reading {
    QByteArray bytes;
    QList<BackgroundOutputPage> pages;
    bool finished = false;
};

// Reads from `cursor` until a page brings nothing and stays put; stops after `pageLimit` pages.
Reading readAll(const BackgroundTaskOutput& output, const QString& task, QString cursor = {},
                const bool textBoundary = false, const int pageLimit = 4000) {
    Reading reading;
    for (int n = 0; n < pageLimit; ++n) {
        const auto page =
            output.read(task, cursor, BackgroundTaskOutput::maximumPageBytes, textBoundary, true);
        if (!page.error.isEmpty())
            return reading;
        reading.pages.append(page);
        reading.bytes.append(page.bytes);
        if (page.bytes.isEmpty() && page.nextCursor == cursor) {
            reading.finished = true;
            return reading;
        }
        cursor = page.nextCursor;
    }
    return reading;
}

QByteArray pattern(const qsizetype size, const int seed = 0) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = char('a' + (i + seed) % 26);
    return bytes;
}

} // namespace

class BackgroundTaskOutputGapTest final : public QObject {
    Q_OBJECT

  private slots:
    void totalTellsAnErrorFromZero() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QVERIFY(store.ready);
        QString error;
        QCOMPARE(store.output->total("job", &error), std::optional<qint64>(0));
        QVERIFY(error.isEmpty());
        QVERIFY(store.output->append("job", "hello", &error));
        QCOMPARE(store.output->total("job"), std::optional<qint64>(5));
        QVERIFY(!store.output->total(QString(), &error));
        QVERIFY(!error.isEmpty());
        QSqlQuery query(store.db());
        QVERIFY(query.exec("DROP TABLE job_output_heads"));
        QVERIFY(!store.output->total("job", &error));
        QVERIFY(!error.isEmpty());
    }

    void appendAtWritesOnlyAtTheExpectedOffset() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QString error;
        QVERIFY2(store.output->appendAt("job", 0, "abc", &error), qPrintable(error));
        QVERIFY(!store.output->appendAt("job", 2, "x", &error)); // below
        QVERIFY(!error.isEmpty());
        QVERIFY(!store.output->appendAt("job", 4, "x", &error)); // above
        QVERIFY(!store.output->appendAt("job", 0, "x", &error));
        QCOMPARE(store.output->total("job"), std::optional<qint64>(3));
        QCOMPARE(readAll(*store.output, "job").bytes, QByteArray("abc"));
        QVERIFY(store.output->appendAt("job", 3, "def", &error));
        QCOMPARE(readAll(*store.output, "job").bytes, QByteArray("abcdef"));
        // Nothing to write still checks the offset.
        QVERIFY(store.output->appendAt("job", 6, {}, &error));
        QVERIFY(!store.output->appendAt("job", 5, {}, &error));
        // A new task starts at 0.
        QVERIFY(!store.output->appendAt("other", 1, "x", &error));
        QCOMPARE(store.output->total("other"), std::optional<qint64>(0));
    }

    void interleavedWritersNeverOverwrite() {
        QTemporaryDir dir;
        Store first(dir.filePath("o.sqlite"));
        Store second(dir.filePath("o.sqlite"));
        QVERIFY(first.ready && second.ready);
        QString error;
        QVERIFY(first.output->appendAt("job", 0, "one", &error));
        QVERIFY(!second.output->appendAt("job", 0, "two", &error));
        const auto seen = second.output->total("job");
        QCOMPARE(seen, std::optional<qint64>(3));
        QVERIFY(second.output->appendAt("job", *seen, "two", &error));
        QVERIFY(!first.output->appendAt("job", 3, "three", &error));
        QVERIFY(!first.output->markGap("job", 3, 10, &error));
        QVERIFY(first.output->appendAt("job", 6, "three", &error));
        QCOMPARE(readAll(*second.output, "job").bytes, QByteArray("onetwothree"));
        QCOMPARE(first.output->total("job"), std::optional<qint64>(11));
    }

    void appendAtBeyondTheLimitKeepsTheTail() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QString error;
        const QByteArray all = pattern(kMax + 37 * 1024 + 5);
        qint64 offset = 0;
        for (qsizetype at = 0; at < all.size(); at += 1024 * 1024) {
            const QByteArray part = all.mid(at, 1024 * 1024);
            QVERIFY2(store.output->appendAt("job", offset, part, &error), qPrintable(error));
            offset += part.size();
        }
        QCOMPARE(store.output->total("job"), std::optional<qint64>(all.size()));
        QCOMPARE(store.stored("job"), kMax);
        const Reading reading = readAll(*store.output, "job");
        QVERIFY(reading.finished);
        QVERIFY(reading.pages.first().truncated);
        QVERIFY(reading.bytes == all.right(kMax));
    }

    void aGapMovesTheOffsetWithoutBytes() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        QString error;
        {
            Store store(path);
            QVERIFY(store.output->appendAt("job", 0, "abc", &error));
            QVERIFY2(store.output->markGap("job", 3, 10, &error), qPrintable(error));
            QCOMPARE(store.output->total("job"), std::optional<qint64>(10));
            QCOMPARE(store.stored("job"), 3);
            // Equal: nothing changes. Below, or from another offset: fails, nothing changes.
            QVERIFY(store.output->markGap("job", 10, 10, &error));
            QVERIFY(!store.output->markGap("job", 10, 9, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!store.output->markGap("job", 3, 20, &error));
            QCOMPARE(store.output->total("job"), std::optional<qint64>(10));
            QCOMPARE(store.chunks("job"), (QList<std::pair<qint64, qint64>>{{0, 3}}));
            // A gap as the first thing of a task.
            QVERIFY(store.output->markGap("fresh", 0, 7, &error));
        }
        Store reopened(path);
        QCOMPARE(reopened.output->total("job"), std::optional<qint64>(10));
        QCOMPARE(reopened.output->total("fresh"), std::optional<qint64>(7));
        QCOMPARE(reopened.stored("fresh"), 0);
        QCOMPARE(readAll(*reopened.output, "job").bytes, QByteArray("abc"));
    }

    void aGapCutsTheTailBelowTheNewFloor() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        QString error;
        const QByteArray all = pattern(kMax);
        {
            Store store(path);
            QVERIFY(store.output->appendAt("job", 0, all, &error));
            const qint64 lost = 1024 * 1024 + 5;
            QVERIFY(store.output->markGap("job", kMax, kMax + lost, &error));
            QCOMPARE(store.stored("job"), kMax - lost);
            QCOMPARE(store.chunks("job").first().first, lost);
        }
        Store reopened(path);
        const Reading reading = readAll(*reopened.output, "job");
        QVERIFY(reading.finished);
        QVERIFY(reading.pages.first().truncated);
        QVERIFY(reading.bytes == all.mid(1024 * 1024 + 5));
        QCOMPARE(reading.pages.last().totalBytes, kMax + 1024 * 1024 + 5);
    }

    void aGapAboveTheLimitLeavesNothing() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        QString error;
        {
            Store store(path);
            QVERIFY(store.output->appendAt("job", 0, pattern(4096), &error));
            QVERIFY(store.output->markGap("job", 4096, 4096 + kMax + 1, &error));
            QCOMPARE(store.stored("job"), 0);
        }
        Store reopened(path);
        QCOMPARE(reopened.output->total("job"), std::optional<qint64>(4096 + kMax + 1));
        QCOMPARE(reopened.stored("job"), 0);
        const auto page = reopened.output->read("job");
        QVERIFY(page.error.isEmpty());
        QVERIFY(page.bytes.isEmpty());
        QVERIFY(page.truncated);
        QCOMPARE(page.totalBytes, 4096 + kMax + 1);
        QVERIFY(reopened.output->appendAt("job", 4096 + kMax + 1, "after", &error));
        QCOMPARE(readAll(*reopened.output, "job").bytes, QByteArray("after"));
    }

    void bytesAfterAGapAreNeverJoined() {
        QTemporaryDir dir;
        const QString path = dir.filePath("o.sqlite");
        QString error;
        {
            Store store(path);
            QVERIFY(store.output->appendAt("job", 0, "abc", &error));
            QVERIFY(store.output->markGap("job", 3, 10, &error));
            QVERIFY(store.output->appendAt("job", 10, "def", &error));
            QVERIFY(store.output->appendAt("job", 13, "g", &error)); // joins its own run
            QCOMPARE(store.chunks("job"), (QList<std::pair<qint64, qint64>>{{0, 3}, {10, 4}}));
        }
        Store reopened(path);
        const auto first = reopened.output->read("job");
        QCOMPARE(first.bytes, QByteArray("abc"));
        QVERIFY(!first.truncated);
        const auto second = reopened.output->read("job", first.nextCursor);
        QCOMPARE(second.bytes, QByteArray("defg"));
        QVERIFY(second.truncated);
        QCOMPARE(second.totalBytes, 14);
        const auto end = reopened.output->read("job", second.nextCursor);
        QVERIFY(end.bytes.isEmpty());
        QVERIFY(!end.truncated);
        QCOMPARE(end.nextCursor, second.nextCursor);
    }

    void aCursorInAGapMovesToTheNextByte() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QString error;
        QVERIFY(store.output->appendAt("job", 0, "abc", &error));
        QVERIFY(store.output->markGap("job", 3, 10, &error));
        QVERIFY(store.output->appendAt("job", 10, "def", &error));
        const auto atStart = store.output->read("job", store.output->read("job").nextCursor);
        QCOMPARE(atStart.bytes, QByteArray("def"));
        QVERIFY(atStart.truncated);
        // A cursor in the middle of the gap, as a reader holding position 5 would have it.
        const QString inside = QString::fromLatin1(
            QJsonDocument(QJsonObject{{"task", "job"}, {"position", "5"}})
                .toJson(QJsonDocument::Compact)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        const auto fromInside = store.output->read("job", inside);
        QVERIFY2(fromInside.error.isEmpty(), qPrintable(fromInside.error));
        QCOMPARE(fromInside.bytes, QByteArray("def"));
        QVERIFY(fromInside.truncated);
        QCOMPARE(fromInside.nextCursor, atStart.nextCursor);
    }

    void aStreamCanEndInAGap() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QString error;
        QVERIFY(store.output->appendAt("job", 0, "abc", &error));
        QVERIFY(store.output->markGap("job", 3, 20, &error));
        const auto first = store.output->read("job");
        QCOMPARE(first.bytes, QByteArray("abc"));
        const auto gap = store.output->read("job", first.nextCursor);
        QVERIFY(gap.bytes.isEmpty());
        QVERIFY(gap.truncated);
        QCOMPARE(gap.totalBytes, 20);
        const auto end = store.output->read("job", gap.nextCursor);
        QVERIFY(end.error.isEmpty());
        QVERIFY(end.bytes.isEmpty());
        QVERIFY(!end.truncated);
        QCOMPARE(end.nextCursor, gap.nextCursor);
        // A cursor past the end still fails.
        const QString beyond = QString::fromLatin1(
            QJsonDocument(QJsonObject{{"task", "job"}, {"position", "21"}})
                .toJson(QJsonDocument::Compact)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        QCOMPARE(store.output->read("job", beyond).error,
                 QStringLiteral("Output cursor is beyond the byte stream."));
    }

    void aPartialCharacterBeforeAGapIsReturned() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QString error;
        const QByteArray sparkle = QStringLiteral("✨").toUtf8();
        const QByteArray before = "x" + sparkle.left(2);
        QVERIFY(store.output->appendAt("job", 0, before, &error));
        QVERIFY(store.output->markGap("job", 3, 9, &error));
        QVERIFY(store.output->appendAt("job", 9, sparkle.mid(2) + "y", &error));
        const auto first = store.output->read("job", {}, 8192, true);
        QCOMPARE(first.bytes, before);
        const Reading reading = readAll(*store.output, "job", {}, true);
        QVERIFY(reading.finished);
        QCOMPARE(reading.bytes, before + sparkle.mid(2) + "y");

        // A full page that ends right before a gap in the middle of a character.
        const QByteArray full = pattern(8190) + sparkle.left(2);
        QVERIFY(store.output->appendAt("page", 0, full, &error));
        QVERIFY(store.output->markGap("page", full.size(), full.size() + 5, &error));
        QVERIFY(store.output->appendAt("page", full.size() + 5, "z", &error));
        const Reading pages = readAll(*store.output, "page", {}, true);
        QVERIFY(pages.finished);
        QCOMPARE(pages.bytes, full + "z");
    }

    void readingManyGapsEnds() {
        QTemporaryDir dir;
        Store store(dir.filePath("o.sqlite"));
        QString error;
        QByteArray kept;
        qint64 offset = 0;
        const QByteArray euro = "\xe2\x82\xac";
        for (int n = 0; n < 60; ++n) {
            QByteArray part = pattern(100 + n * 300, n);
            part.append(euro.left(1 + n % 3)); // often a character cut by the gap
            QVERIFY(store.output->appendAt("job", offset, part, &error));
            kept.append(part);
            offset += part.size();
            QVERIFY(store.output->markGap("job", offset, offset + 1 + n, &error));
            offset += 1 + n;
        }
        for (const bool text : {false, true}) {
            const Reading reading = readAll(*store.output, "job", {}, text);
            QVERIFY(reading.finished);
            QCOMPARE(reading.bytes, kept);
            QCOMPARE(reading.pages.last().totalBytes, offset);
        }
    }
};

QTEST_MAIN(BackgroundTaskOutputGapTest)
#include "BackgroundTaskOutputGapTest.moc"
