#include "runner/entry/RunsOnRequest.h"

#include "runner/jobs/RemoteJobLabels.h"
#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>

namespace ketplus::daemon::runner::entry {

namespace {

// The labels of a runs_on value, sorted without duplicates; nullopt when they do not read.
std::optional<QStringList> labelsOf(const QJsonValue& value) {
    if (!value.isArray())
        return std::nullopt;
    const QJsonArray array = value.toArray();
    if (array.isEmpty() || array.size() > jobs::maximumRunsOn)
        return std::nullopt;
    QStringList labels;
    for (const QJsonValue& label : array) {
        if (!label.isString())
            return std::nullopt;
        labels.append(label.toString());
    }
    labels.removeDuplicates();
    labels = jobs::normalizedRunsOn(labels);
    if (!jobs::isValidRunsOn(labels))
        return std::nullopt;
    return labels;
}

} // namespace

RunsOnRead readRunsOn(const QJsonObject& payload) {
    if (!payload.contains(runsOnKey))
        return {};
    if (payload.value(QStringLiteral("kind")).toString() == QStringLiteral("agent"))
        return {std::nullopt, QStringLiteral("runs_on_unsupported"),
                QStringLiteral("runs_on applies to process tasks only.")};
    auto labels = labelsOf(payload.value(runsOnKey));
    if (!labels)
        return {std::nullopt, QStringLiteral("invalid_labels"),
                QStringLiteral("runs_on must be 1 to 16 labels: os:linux, arch:x86_64, "
                               "arch:arm64 or [a-z0-9][a-z0-9._-]{0,63}.")};
    return {std::move(labels), {}, {}};
}

QJsonObject normalizedRequest(QJsonObject payload) {
    if (const auto labels = labelsOf(payload.value(runsOnKey)))
        payload.insert(runsOnKey, QJsonArray::fromStringList(*labels));
    return payload;
}

QJsonObject withoutRunsOn(QJsonObject payload) {
    payload.remove(runsOnKey);
    return payload;
}

void prepareRetry(BackgroundTaskIntent& intent) {
    const RunsOnRead read = readRunsOn(intent.requestPayload);
    if (!read.ok() || !read.runsOn)
        return;
    intent.runsOn = read.runsOn;
    intent.timeoutMs = qint64(intent.requestPayload.value(QStringLiteral("timeout_ms")).toDouble());
}

} // namespace ketplus::daemon::runner::entry
