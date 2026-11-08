#include "providers/ProviderLineReader.h"

#include <QCoreApplication>
#include <QProcess>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

using namespace ketplus::daemon;

namespace {
qint64 peakResidentBytes() {
#ifdef Q_OS_UNIX
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return -1;
#ifdef Q_OS_MACOS
    return usage.ru_maxrss;
#else
    return qint64(usage.ru_maxrss) * 1024;
#endif
#else
    return -1;
#endif
}

bool scanNested(const int depth, const int chunkBytes, const bool objects = false,
                qint64* growth = nullptr) {
    ProviderLineReader reader(64, {"method", "params.threadId"});
    QList<OversizedProviderLine> skipped;
    QList<QByteArray> lines;
    qsizetype sent = 0;
    const auto append = [&](const QByteArray& bytes) {
        sent += bytes.size();
        reader.append(
            bytes, [&](const QByteArray& line) { lines.append(line); },
            [&](const OversizedProviderLine& line) { skipped.append(line); });
    };
    const qint64 before = peakResidentBytes();
    append("{\"method\":\"turn/completed\",\"params\":{\"payload\":");
    for (int offset = 0; offset < depth; offset += chunkBytes)
        append(objects ? QByteArray("{\"nested\":").repeated(qMin(chunkBytes, depth - offset))
                       : QByteArray(qMin(chunkBytes, depth - offset), '['));
    append("{\"method\":\"spoofed\",\"text\":\"brackets ] } and \\\" quote\"}");
    for (int offset = 0; offset < depth; offset += chunkBytes)
        append(QByteArray(qMin(chunkBytes, depth - offset), objects ? '}' : ']'));
    append(",\"threadId\":\"thread-after\"}}\n");
    const qsizetype firstBytes = sent - 1;
    append("{\"method\":\"next\"}\n");
    if (growth)
        *growth = peakResidentBytes() - before;
    return skipped.size() == 1 && skipped.first().bytes == firstBytes &&
           skipped.first().fields.size() == 2 &&
           skipped.first().fields.value("method").toString() == "turn/completed" &&
           skipped.first().fields.value("params.threadId").toString() == "thread-after" &&
           lines == QList<QByteArray>{"{\"method\":\"next\"}"};
}
} // namespace

class ProviderLineReaderTest final : public QObject {
    Q_OBJECT
  private slots:
    void nestingKeepsRoutingAndNextLine_data() {
        QTest::addColumn<int>("depth");
        QTest::addColumn<int>("chunk");
        QTest::addColumn<bool>("objects");
        for (const int depth : {60, 61, 62, 63, 64, 65, 4096})
            for (const int chunk : {1, 7, 1024})
                for (const bool objects : {false, true})
                    QTest::newRow(qPrintable(
                        QString("depth-%1-chunk-%2-objects-%3").arg(depth).arg(chunk).arg(objects)))
                        << depth << chunk << objects;
    }
    void nestingKeepsRoutingAndNextLine() {
        QFETCH(int, depth);
        QFETCH(int, chunk);
        QFETCH(bool, objects);
        QVERIFY(scanNested(depth, chunk, objects));
    }
    void deepDiscardUsesBoundedMemory() {
#ifndef Q_OS_UNIX
        QSKIP("Peak resident-memory probe uses POSIX getrusage.");
#else
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"--deep-scan-probe"});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(30000));
        const QByteArray output = child.readAll();
        qInfo().noquote() << output;
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, output.constData());
#endif
    }
    void clearDuringDiscardDropsOldDepthAndRouting() {
        ProviderLineReader reader(16, {"method"});
        QList<QByteArray> lines;
        QList<OversizedProviderLine> skipped;
        const auto line = [&](const QByteArray& value) { lines.append(value); };
        const auto oversized = [&](const OversizedProviderLine& value) { skipped.append(value); };
        reader.append(QByteArray(4096, '['), line, oversized);
        reader.clear();
        reader.append("{\"method\":\"new\"}\n", line, oversized);
        QCOMPARE(lines, QList<QByteArray>{"{\"method\":\"new\"}"});
        QVERIFY(skipped.isEmpty());
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--deep-scan-probe")) {
        qint64 growth = 0;
        const bool routed = scanNested(2 * 1024 * 1024, 1024, false, &growth);
        qInfo() << "deep scan resident growth bytes:" << growth << "routing:" << routed;
        return routed && growth >= 0 && growth < 16 * 1024 * 1024 ? 0 : 1;
    }
    ProviderLineReaderTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "ProviderLineReaderTest.moc"
