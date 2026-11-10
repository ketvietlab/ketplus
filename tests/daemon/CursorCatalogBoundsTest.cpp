#include "providers/CursorAdapter.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
constexpr qsizetype outputLimit = 1024 * 1024;
const QByteArray validCatalog = "fixture-model - Fixture Model\n";

int emitCatalog(const QString& shape) {
    QFile output;
    QFile error;
    if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle) ||
        !error.open(stderr, QIODevice::WriteOnly, QFile::DontCloseHandle))
        return 2;
    const auto write = [](QFile& file, const QByteArray& bytes) {
        return file.write(bytes) == bytes.size() && file.flush();
    };
    if (!write(output, validCatalog))
        return 2;
    if (shape == "stdout-boundary" || shape == "stdout-overflow") {
        if (!write(output, QByteArray(outputLimit - validCatalog.size() +
                                          (shape == "stdout-overflow" ? 1 : 0),
                                      '\n')))
            return 2;
    } else if (shape == "stderr-boundary" || shape == "stderr-overflow") {
        if (!write(error, QByteArray(outputLimit - validCatalog.size() +
                                         (shape == "stderr-overflow" ? 1 : 0),
                                     'x')))
            return 2;
    } else if (shape == "stream-stdout" || shape == "stream-stderr") {
        if (!write(shape == "stream-stdout" ? output : error, QByteArray(512 * 1024, '\n')))
            return 2;
        QThread::msleep(700);
    } else if (shape == "flood-stdout" || shape == "flood-stderr") {
        for (int i = 0; i < 1024; ++i)
            if (!write(shape == "flood-stdout" ? output : error, QByteArray(64 * 1024, 'x')))
                return 2;
        QFile completed(qEnvironmentVariable("KETPLUS_TEST_CATALOG_COMPLETED_FILE"));
        if (!completed.open(QIODevice::WriteOnly) || completed.write("done") != 4)
            return 2;
    } else if (shape == "timeout") {
        QThread::sleep(20);
    }
    return shape == "failed-exit" ? 7 : 0;
}
} // namespace

class CursorCatalogBoundsTest final : public QObject {
    Q_OBJECT
  private slots:
    void catalogOutput_data() {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<bool>("accepted");
        for (const QString& shape :
             {"valid", "stdout-boundary", "stderr-boundary", "stdout-overflow", "stderr-overflow",
              "failed-exit", "flood-stdout", "flood-stderr"}) {
            QTest::newRow(qPrintable(shape))
                << shape << (shape == "valid" || shape.endsWith("boundary"));
        }
    }
    void catalogOutput() {
        QFETCH(QString, shape);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray previous = qgetenv("KETPLUS_TEST_CATALOG_SHAPE");
        const QByteArray previousMarker = qgetenv("KETPLUS_TEST_CATALOG_COMPLETED_FILE");
        const auto restore = qScopeGuard([&] {
            qputenv("KETPLUS_TEST_CATALOG_SHAPE", previous);
            qputenv("KETPLUS_TEST_CATALOG_COMPLETED_FILE", previousMarker);
        });
        qputenv("KETPLUS_TEST_CATALOG_SHAPE", shape.toUtf8());
        const QString completed = directory.filePath("completed");
        qputenv("KETPLUS_TEST_CATALOG_COMPLETED_FILE", completed.toUtf8());
        CursorAdapter adapter(QCoreApplication::applicationFilePath());
        int calls = 0;
        QJsonArray catalog;
        adapter.call("model/list", {}, [&](const QJsonObject& result, const QString& error) {
            QVERIFY(error.isEmpty());
            catalog = result.value("data").toArray();
            ++calls;
        });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 10000);
        if (shape.startsWith("flood-"))
            QVERIFY2(!QFile::exists(completed), "The producer completed its unbounded flood");
        const QJsonArray expected =
            accepted ? CursorAdapter::parseModelList(QString::fromUtf8(validCatalog))
                     : CursorAdapter::modelCatalog();
        QCOMPARE(catalog, expected);
        // A cached request must see the same bounded result.
        adapter.call("model/list", {}, [&](const QJsonObject& result, const QString& error) {
            QVERIFY(error.isEmpty());
            QCOMPARE(result.value("data").toArray(), expected);
            ++calls;
        });
        QCOMPARE(calls, 2);
    }
    void channelsAreDrainedBeforeExit_data() {
        QTest::addColumn<QString>("shape");
        QTest::newRow("stdout") << QString("stream-stdout");
        QTest::newRow("stderr") << QString("stream-stderr");
    }
    void channelsAreDrainedBeforeExit() {
        QFETCH(QString, shape);
        const QByteArray previous = qgetenv("KETPLUS_TEST_CATALOG_SHAPE");
        const auto restore = qScopeGuard([&] { qputenv("KETPLUS_TEST_CATALOG_SHAPE", previous); });
        qputenv("KETPLUS_TEST_CATALOG_SHAPE", shape.toUtf8());
        CursorAdapter adapter(QCoreApplication::applicationFilePath());
        int calls = 0;
        for (int i = 0; i < 3; ++i)
            adapter.call("model/list", {}, [&](const QJsonObject&, const QString&) { ++calls; });
        QProcess* catalog = nullptr;
        for (QProcess* child : adapter.findChildren<QProcess*>())
            if (child->arguments() == QStringList{"models"})
                catalog = child;
        QVERIFY(catalog != nullptr);
        bool observed = false;
        qint64 retained = 0;
        const auto inspect = [&] {
            observed = true;
            catalog->setReadChannel(shape == "stream-stdout" ? QProcess::StandardOutput
                                                             : QProcess::StandardError);
            retained = qMax(retained, catalog->bytesAvailable());
        };
        if (shape == "stream-stdout")
            connect(catalog, &QProcess::readyReadStandardOutput, this, inspect);
        else
            connect(catalog, &QProcess::readyReadStandardError, this, inspect);
        QTRY_COMPARE_WITH_TIMEOUT(calls, 3, 10000);
        QVERIFY(observed);
        QCOMPARE(retained, 0);
    }
    void timeoutSettlesAllWaitersOnce() {
        const QByteArray previous = qgetenv("KETPLUS_TEST_CATALOG_SHAPE");
        const auto restore = qScopeGuard([&] { qputenv("KETPLUS_TEST_CATALOG_SHAPE", previous); });
        qputenv("KETPLUS_TEST_CATALOG_SHAPE", "timeout");
        CursorAdapter adapter(QCoreApplication::applicationFilePath());
        int calls = 0;
        QList<QJsonArray> results;
        for (int i = 0; i < 3; ++i)
            adapter.call("model/list", {}, [&](const QJsonObject& result, const QString&) {
                results.append(result.value("data").toArray());
                ++calls;
            });
        QTimer* deadline = nullptr;
        for (QTimer* timer : adapter.findChildren<QTimer*>()) {
            if (timer->isActive() && timer->interval() == 15000) {
                deadline = timer;
                timer->stop();
            }
        }
        QVERIFY(deadline != nullptr);
        QProcess* catalog = nullptr;
        for (QProcess* child : adapter.findChildren<QProcess*>())
            if (child->arguments() == QStringList{"models"})
                catalog = child;
        QVERIFY(catalog != nullptr);
        // Start the timeout only after a real partial result was written. This avoids
        // accidentally testing FailedToStart/empty output on a loaded host.
        connect(catalog, &QProcess::readyReadStandardOutput, deadline,
                [deadline] { deadline->start(50); });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 3, 10000);
        for (const QJsonArray& result : results)
            QCOMPARE(result, CursorAdapter::modelCatalog());
        adapter.stop();
        QCOMPARE(calls, 3);
    }
    void failedLaunchSettlesAllWaiters() {
        CursorAdapter adapter(QCoreApplication::applicationFilePath() + ".missing");
        int calls = 0;
        for (int i = 0; i < 3; ++i)
            adapter.call("model/list", {}, [&](const QJsonObject& result, const QString& error) {
                ++calls;
                QVERIFY(error.isEmpty());
                QCOMPARE(result.value("data").toArray(), CursorAdapter::modelCatalog());
            });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 3, 10000);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().mid(1) == QStringList{"models"})
        return emitCatalog(qEnvironmentVariable("KETPLUS_TEST_CATALOG_SHAPE"));
    CursorCatalogBoundsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "CursorCatalogBoundsTest.moc"
