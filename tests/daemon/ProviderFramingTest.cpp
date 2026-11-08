#include "providers/ClaudeAdapter.h"
#include "providers/CursorAdapter.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QSignalBlocker>
#include <QtTest>

#include <memory>

using namespace ketplus::daemon;

namespace {
constexpr qsizetype limit = 16 * 1024 * 1024;

bool emitFixture(const QString& shape) {
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly, QFile::DontCloseHandle))
        return false;
    const auto write = [&](const QByteArray& value) { return output.write(value) == value.size(); };
    const auto line = [&](qsizetype bytes, bool newline) {
        const QByteArray prefix = "{\"id\":73,\"method\":\"fixture\",\"padding\":\"";
        return write(prefix) && write(QByteArray(bytes - prefix.size() - 2, 'x')) &&
               write(newline ? QByteArray("\"}\n") : QByteArray("\"}"));
    };
    bool ok = false;
    if (shape == "exact-limit")
        ok = line(limit, true);
    else if (shape == "many-valid")
        ok = line(limit / 2 + 1, true) && line(limit / 2 + 1, true);
    else if (shape == "oversized-newline")
        ok = line(limit + 1, true);
    else if (shape == "oversized-partial")
        ok = line(limit + 1, false);
    else if (shape == "valid-prefix-newline")
        ok = write("{\"id\":11,\"method\":\"fixture\"}\n") && line(limit + 1, true) &&
             write("{\"id\":99,\"method\":\"fixture\"}\n");
    else if (shape == "valid-prefix-partial")
        ok = write("{\"id\":11,\"method\":\"fixture\"}\n") && line(limit + 1, false);
    else if (shape == "blank-prefix-partial")
        ok = write("\n\n") && line(limit + 1, false);
    else if (shape == "small-valid")
        ok = write("{\"id\":101,\"method\":\"fixture\"}\n");
    return ok && output.flush();
}
} // namespace

class ProviderFramingTest final : public QObject {
    Q_OBJECT
  private slots:
    void coalescedOutputEnforcesEachRawLine_data() {
        QTest::addColumn<bool>("claude");
        QTest::addColumn<QString>("shape");
        QTest::addColumn<bool>("oversized");
        for (const bool claude : {false, true}) {
            for (const QString& shape :
                 {"exact-limit", "many-valid", "oversized-newline", "oversized-partial",
                  "valid-prefix-newline", "valid-prefix-partial", "blank-prefix-partial"}) {
                QTest::newRow(qPrintable(QString("%1-%2").arg(claude ? "claude" : "cursor", shape)))
                    << claude << shape << (shape != "exact-limit" && shape != "many-valid");
            }
        }
    }
    void coalescedOutputEnforcesEachRawLine() {
        QFETCH(bool, claude);
        QFETCH(QString, shape);
        QFETCH(bool, oversized);
        std::unique_ptr<ProviderAdapter> adapter;
        if (claude)
            adapter = std::make_unique<ClaudeAdapter>("unused-fixture-provider");
        else
            adapter = std::make_unique<CursorAdapter>("unused-fixture-provider");
        QSignalSpy stops(adapter.get(), &ProviderAdapter::stopped);
        QSignalSpy requests(adapter.get(), &ProviderAdapter::requestReceived);
        const auto processes =
            adapter->findChildren<QProcess*>(QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(processes.size(), 1);
        QProcess* process = processes.first();
        {
            // Coalesce real QProcess output before notifying the adapter. This also keeps
            // provider initialization/exit callbacks out of the deterministic framing test.
            const QSignalBlocker blocker(process);
            process->start(QCoreApplication::applicationFilePath(),
                           {"--emit-provider-stdout", shape});
            QVERIFY(process->waitForStarted());
            QVERIFY(process->waitForFinished(30000));
            QCOMPARE(process->exitStatus(), QProcess::NormalExit);
            QCOMPARE(process->exitCode(), 0);
            QVERIFY(process->bytesAvailable() >= limit);
        }
        QVERIFY(
            QMetaObject::invokeMethod(process, "readyReadStandardOutput", Qt::DirectConnection));
        QCOMPARE(stops.count(), oversized ? 1 : 0);
        if (oversized)
            QVERIFY(stops.first().first().toString().contains("oversized message"));
        QCOMPARE(process->bytesAvailable(), 0);
        const int expectedRequests = shape.startsWith("valid-prefix") ? 1
                                     : shape == "many-valid"          ? 2
                                     : shape == "exact-limit"         ? 1
                                                                      : 0;
        QCOMPARE(requests.count(), expectedRequests);
        if (shape.startsWith("valid-prefix"))
            QCOMPARE(requests.first().first().value<QJsonValue>().toInt(), 11);
        // A later notification cannot publish the quarantined suffix or fail twice.
        QVERIFY(
            QMetaObject::invokeMethod(process, "readyReadStandardOutput", Qt::DirectConnection));
        QCOMPARE(requests.count(), expectedRequests);
        QCOMPARE(stops.count(), oversized ? 1 : 0);
        // QProcess reopens its channel on a fresh generation; no discarded prefix survives.
        {
            const QSignalBlocker blocker(process);
            process->start(QCoreApplication::applicationFilePath(),
                           {"--emit-provider-stdout", "small-valid"});
            QVERIFY(process->waitForStarted());
            QVERIFY(process->waitForFinished(30000));
            QCOMPARE(process->exitCode(), 0);
        }
        QVERIFY(
            QMetaObject::invokeMethod(process, "readyReadStandardOutput", Qt::DirectConnection));
        QCOMPARE(stops.count(), oversized ? 1 : 0);
        QCOMPARE(requests.count(), expectedRequests + 1);
        QCOMPARE(requests.last().first().value<QJsonValue>().toInt(), 101);
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 3 && args[1] == "--emit-provider-stdout")
        return emitFixture(args[2]) ? 0 : 1;
    ProviderFramingTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "ProviderFramingTest.moc"
