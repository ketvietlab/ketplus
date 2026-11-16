#include "storage/BackgroundTaskOutput.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

class BackgroundTaskOutputTest final : public QObject {
    Q_OBJECT

  private slots:
    void roundTripAcrossPagesAndReopen() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("output.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskOutput output(db);
            QString error;
            QVERIFY2(output.initialize(&error), qPrintable(error));
            QByteArray source;
            for (int i = 0; i < 9000; ++i)
                source.append("\xe2\x82\xac");
            QVERIFY2(output.append("job-a", source, &error), qPrintable(error));
            QByteArray received;
            QString cursor;
            do {
                const auto page = output.read("job-a", cursor, 100000);
                QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
                QVERIFY(page.bytes.size() <= 8192);
                QCOMPARE(page.totalBytes, source.size());
                QVERIFY(!page.truncated);
                received.append(page.bytes);
                cursor = page.nextCursor;
            } while (received.size() < source.size());
            QCOMPARE(received, source); // Raw byte pages can split UTF-8; reconstruction is exact.
            QCOMPARE(output.read("job-a", cursor).bytes.size(), 0);
            QVERIFY(!output.read("job-b", cursor).error.isEmpty());
            QVERIFY(!output.read("job-a", "invalid!").error.isEmpty());
        }
        QSqlDatabase::removeDatabase(name);
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(directory.filePath("output.sqlite"));
            QVERIFY(db.open());
            BackgroundTaskOutput output(db);
            const auto page = output.read("job-a");
            QVERIFY(page.error.isEmpty());
            QCOMPARE(page.bytes.size(), 8192);
            QCOMPARE(page.totalBytes, 27000);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void ringIsBoundedAndReportsLostBytes() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskOutput output(db);
            QString error;
            QVERIFY(output.initialize(&error));
            QVERIFY(output.append("job", QByteArray(8192, 'a'), &error));
            const auto oldCursor = output.read("job").nextCursor;
            QByteArray tail(BackgroundTaskOutput::maximumBytes + 37, 'b');
            tail.replace(tail.size() - 4, 4, "TAIL");
            QVERIFY2(output.append("job", tail, &error), qPrintable(error));
            const auto page = output.read("job", oldCursor);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QVERIFY(page.truncated);
            QCOMPARE(page.totalBytes, 8192 + tail.size());
            QCOMPARE(page.bytes, QByteArray(8192, 'b'));
            QSqlQuery query(db);
            QVERIFY(query.exec("SELECT sum(length(bytes)),count(*) FROM job_output_chunks"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toLongLong(), BackgroundTaskOutput::maximumBytes);
            QVERIFY(query.value(1).toInt() <= 1281);
            QByteArray received = page.bytes;
            QString cursor = page.nextCursor;
            while (true) {
                const auto next = output.read("job", cursor);
                QVERIFY(next.error.isEmpty());
                if (next.bytes.isEmpty())
                    break;
                received.append(next.bytes);
                cursor = next.nextCursor;
            }
            QCOMPARE(received, tail.right(BackgroundTaskOutput::maximumBytes));
            // Advance the floor inside a retained chunk and merge a short tail.
            const QByteArray suffix(9137, 'c');
            QVERIFY(output.append("job", suffix, &error));
            const auto wrapped = output.read("job", page.nextCursor);
            QVERIFY(wrapped.error.isEmpty());
            QVERIFY(wrapped.truncated);
            QCOMPARE(wrapped.totalBytes, 8192 + tail.size() + suffix.size());
            received.clear();
            cursor.clear();
            while (true) {
                const auto next = output.read("job", cursor);
                QVERIFY(next.error.isEmpty());
                if (next.bytes.isEmpty())
                    break;
                received.append(next.bytes);
                cursor = next.nextCursor;
            }
            QCOMPARE(received, (tail + suffix).right(BackgroundTaskOutput::maximumBytes));
        }
        QSqlDatabase::removeDatabase(name);
    }

    void tinyWritesCoalesceAndFailedAppendRollsBack() {
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(":memory:");
            QVERIFY(db.open());
            BackgroundTaskOutput output(db);
            QString error;
            QVERIFY(output.initialize(&error));
            for (int i = 0; i < 1000; ++i)
                QVERIFY2(output.append("job", "x", &error), qPrintable(error));
            QSqlQuery query(db);
            QVERIFY(query.exec("SELECT count(*) FROM job_output_chunks"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
            QVERIFY(query.exec("CREATE TRIGGER fail_output_head BEFORE UPDATE ON job_output_heads "
                               "BEGIN SELECT RAISE(ABORT,'injected failure'); END"));
            QVERIFY(!output.append("job", "lost", &error));
            QVERIFY(error.contains("injected failure"));
            const auto page = output.read("job");
            QVERIFY(page.error.isEmpty());
            QCOMPARE(page.totalBytes, 1000);
            QCOMPARE(page.bytes, QByteArray(1000, 'x'));
        }
        QSqlDatabase::removeDatabase(name);
    }
};

QTEST_GUILESS_MAIN(BackgroundTaskOutputTest)
#include "BackgroundTaskOutputTest.moc"
