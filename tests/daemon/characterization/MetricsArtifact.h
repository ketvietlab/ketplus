#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>

namespace r03 {
inline QString metricsStatus(int testExitCode, const QSet<QString>& completedTests,
                             const QJsonArray& rows) {
    if (testExitCode != 0)
        return "failed";
    const QSet<QString> required{"cloudLifecycle", "workloadCommandAdapter",
                                 "pagingGenerationDuplicatesAndReopen",
                                 "productionWorkloadSamples"};
    if (completedTests != required || rows.size() != 9)
        return "partial";
    const QSet<QString> scenarios{"items-1k", "items-10k", "giant-turn"};
    QSet<QString> seen;
    for (const auto& value : rows) {
        const auto row = value.toObject();
        const auto scenario = row.value("scenario").toString();
        const auto sample = row.value("sample");
        if (!scenarios.contains(scenario) || !sample.isDouble() || sample.toDouble() < 0 ||
            sample.toDouble() > 2 || sample.toDouble() != sample.toInt())
            return "partial";
        const auto key = scenario + ':' + QString::number(sample.toInt());
        if (seen.contains(key))
            return "partial";
        seen.insert(key);
    }
    return "complete";
}

inline bool writeAll(QIODevice& device, const QByteArray& bytes) {
    qint64 written = 0;
    while (written < bytes.size()) {
        const auto count = device.write(bytes.constData() + written, bytes.size() - written);
        if (count <= 0)
            return false;
        written += count;
    }
    return written == bytes.size();
}

inline bool publishMetrics(const QString& output, const QJsonObject& report, QString* error) {
    QSaveFile file(output);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(report).toJson();
    if (!file.open(QIODevice::WriteOnly) || !writeAll(file, bytes)) {
        *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
} // namespace r03
