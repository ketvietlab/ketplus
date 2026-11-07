#pragma once
#include <QJsonObject>
#include <QString>
#include <limits>

namespace ketplus {
struct FileChangeStatistics {
    int added{0};
    int removed{0};
    bool known{false};
};
// QtCore-only contract shared by journal compaction and transcript presentation.
inline FileChangeStatistics rawFileChangeStatistics(const QJsonObject& change) {
    const auto value = change.value(QStringLiteral("diff"));
    const auto diff = value.toString();
    const auto kind =
        change.value(QStringLiteral("kind")).toObject().value(QStringLiteral("type")).toString();
    FileChangeStatistics result;
    if (value.isString() && (kind == QStringLiteral("add") || kind == QStringLiteral("delete"))) {
        const int lines =
            diff.isEmpty() ? 0 : int(diff.count('\n')) + (diff.endsWith('\n') ? 0 : 1);
        result.added = kind == QStringLiteral("add") ? lines : 0;
        result.removed = kind == QStringLiteral("delete") ? lines : 0;
        result.known = true;
        return result;
    }
    bool inHunk = false;
    for (const auto& line : diff.split('\n')) {
        if (line.startsWith(QStringLiteral("@@ "))) {
            inHunk = true;
            result.known = true;
        } else if (line.startsWith(QStringLiteral("diff --git ")))
            inHunk = false;
        else if (inHunk && line.startsWith('+'))
            ++result.added;
        else if (inHunk && line.startsWith('-'))
            ++result.removed;
    }
    return result;
}
inline FileChangeStatistics fileChangeStatistics(const QJsonObject& change) {
    const auto marker = change.value(QStringLiteral("compacted")).toObject();
    if (marker.value(QStringLiteral("policy")) != QStringLiteral("file-diff-v1"))
        return rawFileChangeStatistics(change);
    const auto stats = marker.value(QStringLiteral("stats")).toObject();
    const auto validCount = [](const QJsonValue& value) {
        return value.isDouble() && value.toDouble() >= 0 &&
               value.toDouble() <= std::numeric_limits<int>::max() &&
               value.toDouble() == value.toInt(-1);
    };
    if (stats.value(QStringLiteral("known")) != true ||
        !validCount(stats.value(QStringLiteral("added"))) ||
        !validCount(stats.value(QStringLiteral("removed"))))
        return {}; // never present excerpt counts as complete statistics
    return {stats.value(QStringLiteral("added")).toInt(),
            stats.value(QStringLiteral("removed")).toInt(), true};
}
} // namespace ketplus
