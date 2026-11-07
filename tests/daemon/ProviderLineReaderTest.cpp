#include "providers/ProviderLineReader.h"

#include <QCoreApplication>
#include <QJsonObject>
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
    void routingStringsUseJsonEncoding_data();
    void routingStringsUseJsonEncoding();
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

#ifndef Q_MOC_RUN
void ProviderLineReaderTest::routingStringsUseJsonEncoding_data() {
    QTest::addColumn<QByteArray>("fields");
    QTest::addColumn<QJsonObject>("expected");
    QTest::addColumn<int>("chunk");
    const auto add = [](const char* name, const QByteArray& fields, const QJsonObject& expected) {
        for (const int chunk : {1, 7, 1024})
            QTest::newRow(qPrintable(QString("%1-chunk-%2").arg(name).arg(chunk)))
                << fields << expected << chunk;
    };
    const QJsonObject normal{{"method", "turn/completed"}, {"params.threadId", "thread-1"}};
    add("plain", R"("method":"turn/completed","params":{"threadId":"thread-1"})", normal);
    add("escaped-values", R"("method":"turn\/completed","params":{"threadId":"\u0074hread-1"})",
        normal);
    add("escaped-keys",
        R"("\u006dethod":"turn/completed","pa\u0072ams":{"thre\u0061dId":"thread-1"})", normal);
    add("escaped-characters",
        R"("method":"turn/completed","params":{"threadId":"tab\tquote\"slash\\"})",
        {{"method", "turn/completed"}, {"params.threadId", "tab\tquote\"slash\\"}});
    add("unicode-pair", R"("params":{"threadId":"\ud83d\ude00"})",
        {{"params.threadId", QString::fromUtf8("😀")}});
    add("utf8",
        QByteArray("\"params\":{\"threadId\":\"") + QString::fromUtf8("luồng-😀").toUtf8() + "\"}",
        {{"params.threadId", QString::fromUtf8("luồng-😀")}});
    add("invalid-escape", R"("method":"bad\q","params":{"threadId":"thread-1"})", {});
    add("invalid-utf8",
        QByteArray("\"method\":\"turn/completed\",\"params\":{\"threadId\":\"bad") + char(0xff) +
            "\"}",
        {});
    add("unescaped-control",
        QByteArray("\"method\":\"bad") + char(1) + "\",\"params\":{\"threadId\":\"thread-1\"}", {});
    add("literal-dot-key",
        R"("params.threadId":"spoof","params":{"threadId":"thread-1","turn.id":"spoof-turn","turn":{"id":"turn-1"}})",
        {{"params.threadId", "thread-1"}, {"params.turn.id", "turn-1"}});
    add("numeric-id", R"("id":7)", {{"id", 7}});
    add("invalid-method-with-id", R"("id":7,"method":"bad\q")", {});
    add("invalid-method-before-id", R"("method":"bad\q","id":7)", {});
    add("field-limit", QByteArray("\"params\":{\"threadId\":\"") + QByteArray(1024, 'x') + "\"}",
        {{"params.threadId", QString(1024, 'x')}});
    add("field-over-limit",
        QByteArray("\"params\":{\"threadId\":\"") + QByteArray(1025, 'x') + "\"}", {});
}
void ProviderLineReaderTest::routingStringsUseJsonEncoding() {
    QFETCH(QByteArray, fields);
    QFETCH(QJsonObject, expected);
    QFETCH(int, chunk);
    ProviderLineReader reader(64, {"id", "method", "params.threadId", "params.turn.id"});
    QList<OversizedProviderLine> skipped;
    QList<QByteArray> lines;
    const QByteArray next =
        QByteArray("{\"padding\":\"") + QByteArray(200, 'x') + "\",\"method\":\"after\"}\n";
    const QByteArray input = QByteArray("{\"padding\":\"") + QByteArray(200, 'x') + "\"," + fields +
                             "}\n" + next + "{}\n";
    for (qsizetype offset = 0; offset < input.size(); offset += chunk)
        reader.append(
            input.mid(offset, chunk), [&](const QByteArray& line) { lines.append(line); },
            [&](const OversizedProviderLine& line) { skipped.append(line); });
    QCOMPARE(skipped.size(), 2);
    QJsonObject actual;
    for (auto it = skipped.first().fields.cbegin(); it != skipped.first().fields.cend(); ++it)
        actual.insert(it.key(), it.value());
    QCOMPARE(actual, expected);
    QCOMPARE(skipped.first().bytes, input.indexOf('\n'));
    QCOMPARE(skipped.at(1).fields.value("method").toString(), QString("after"));
    QCOMPARE(skipped.at(1).bytes, next.size() - 1);
    QCOMPARE(lines, QList<QByteArray>{"{}"});
}
#endif

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
