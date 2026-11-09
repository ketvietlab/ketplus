#include "MetricsArtifact.h"
#include <QTemporaryDir>
#include <QtTest>

class MetricsArtifactTest final : public QObject {
    Q_OBJECT
    const QSet<QString> required_{"cloudLifecycle", "workloadCommandAdapter",
                                  "pagingGenerationDuplicatesAndReopen",
                                  "productionWorkloadSamples"};
    QJsonArray rows() {
        QJsonArray result;
        for (const auto& scenario :
             {QString("items-1k"), QString("items-10k"), QString("giant-turn")})
            for (int sample = 0; sample < 3; ++sample)
                result.append(QJsonObject{{"scenario", scenario}, {"sample", sample}});
        return result;
    }
  private slots:
    void complete() { QCOMPARE(r03::metricsStatus(0, required_, rows()), "complete"); }
    void failedRun() { QCOMPARE(r03::metricsStatus(1, required_, rows()), "failed"); }
    void partialRun() {
        auto missing = rows();
        missing.removeLast();
        QCOMPARE(r03::metricsStatus(0, required_, missing), "partial");
        QCOMPARE(r03::metricsStatus(0, {}, rows()), "partial");
    }
    void duplicateUnknownAndInvalidSample() {
        auto duplicate = rows();
        duplicate[8] = duplicate[7];
        QCOMPARE(r03::metricsStatus(0, required_, duplicate), "partial");
        for (const QJsonObject& bad : {QJsonObject{{"scenario", "unknown"}, {"sample", 2}},
                                       QJsonObject{{"scenario", "giant-turn"}, {"sample", 0.5}},
                                       QJsonObject{{"scenario", "giant-turn"}, {"sample", "2"}}}) {
            auto invalid = rows();
            invalid[8] = bad;
            QCOMPARE(r03::metricsStatus(0, required_, invalid), "partial");
        }
    }
    void atomicPublicationAndWriteFailure() {
        QTemporaryDir directory;
        const auto path = directory.filePath("metrics.json");
        QString error;
        const QJsonObject report{{"status", "complete"}, {"rows", rows()}};
        QVERIFY(r03::publishMetrics(path, report, &error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QJsonDocument(report).toJson());
        QVERIFY(!r03::publishMetrics(directory.filePath("missing/metrics.json"), report, &error));
        QVERIFY(!r03::publishMetrics(directory.path(), report, &error));
    }
    void shortWriteFails() {
        class ShortDevice : public QIODevice {
            qint64 readData(char*, qint64) override { return -1; }
            qint64 writeData(const char*, qint64) override { return 0; }
        } device;
        QVERIFY(device.open(QIODevice::WriteOnly));
        QVERIFY(!r03::writeAll(device, "must write every byte"));
    }
};
QTEST_GUILESS_MAIN(MetricsArtifactTest)
#include "MetricsArtifactTest.moc"
