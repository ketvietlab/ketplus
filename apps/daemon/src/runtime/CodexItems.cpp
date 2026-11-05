#include "runtime/CodexItems.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <utility>

namespace ketplus::daemon {

namespace {

QString fileName(const QJsonObject& action) {
    const QString name = action.value(QStringLiteral("name")).toString();
    return name.isEmpty() ? QFileInfo(action.value(QStringLiteral("path")).toString()).fileName()
                          : name;
}

} // namespace

QJsonObject withCommandDescription(QJsonObject item) {
    if (item.value(QStringLiteral("type")).toString() != QStringLiteral("commandExecution") ||
        !item.value(QStringLiteral("description")).toString().trimmed().isEmpty())
        return item;
    QStringList reads;
    QStringList phrases;
    for (const QJsonValue& value : item.value(QStringLiteral("commandActions")).toArray()) {
        const QJsonObject action = value.toObject();
        const QString type = action.value(QStringLiteral("type")).toString();
        const QString path = action.value(QStringLiteral("path")).toString();
        if (type == QStringLiteral("read")) {
            if (const QString name = fileName(action); !name.isEmpty() && !reads.contains(name))
                reads.append(name);
        } else if (type == QStringLiteral("search")) {
            const QString query = action.value(QStringLiteral("query")).toString();
            QString phrase = query.isEmpty() ? QStringLiteral("Search files")
                                             : QStringLiteral("Search for “%1”").arg(query);
            if (!path.isEmpty())
                phrase += QStringLiteral(" in %1").arg(QFileInfo(path).fileName().isEmpty()
                                                             ? path
                                                             : QFileInfo(path).fileName());
            phrases.append(phrase);
        } else if (type == QStringLiteral("listFiles")) {
            phrases.append(path.isEmpty() ? QStringLiteral("List files")
                                          : QStringLiteral("List files in %1").arg(path));
        }
    }
    if (reads.size() == 1)
        phrases.prepend(QStringLiteral("Read %1").arg(reads.constFirst()));
    else if (reads.size() > 1)
        phrases.prepend(QStringLiteral("Read %1 files").arg(reads.size()));
    if (!phrases.isEmpty())
        item.insert(QStringLiteral("description"), phrases.join(QStringLiteral(", ")));
    return item;
}

QJsonObject withWorktreeChangePaths(QJsonObject item, const QString& worktree) {
    if (item.value(QStringLiteral("type")).toString() != QStringLiteral("fileChange") ||
        worktree.isEmpty())
        return item;
    const QDir root(QDir::cleanPath(worktree));
    QJsonArray changes;
    for (const QJsonValue& value : item.value(QStringLiteral("changes")).toArray()) {
        QJsonObject change = value.toObject();
        const QString path = change.value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            const QString relative =
                root.relativeFilePath(QDir::cleanPath(root.absoluteFilePath(path)));
            if (relative.isEmpty() || relative == QStringLiteral(".") ||
                relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
                QDir::isAbsolutePath(relative))
                change.insert(QStringLiteral("outsideWorktree"), true);
            else
                change.insert(QStringLiteral("worktreePath"), relative);
        }
        changes.append(change);
    }
    item.insert(QStringLiteral("changes"), changes);
    return item;
}

QJsonArray codexPlanLimits(const QJsonObject& params) {
    QJsonObject bucket = params.value(QStringLiteral("rateLimitsByLimitId"))
                             .toObject()
                             .value(QStringLiteral("codex"))
                             .toObject();
    if (bucket.isEmpty()) {
        bucket = params.value(QStringLiteral("rateLimits")).toObject();
        const QString id = bucket.value(QStringLiteral("limitId")).toString();
        if (!id.isEmpty() && id != QStringLiteral("codex"))
            return {};
    }
    QJsonArray limits;
    for (const QString& key : {QStringLiteral("primary"), QStringLiteral("secondary")}) {
        const QJsonObject window = bucket.value(key).toObject();
        const qint64 minutes = window.value(QStringLiteral("windowDurationMins")).toInteger(0);
        if (minutes <= 0 || !window.value(QStringLiteral("usedPercent")).isDouble())
            continue;
        const auto [id, label] =
            minutes == 300
                ? std::pair{QStringLiteral("five_hour"), QStringLiteral("Session · 5 hours")}
            : minutes == 10080
                ? std::pair{QStringLiteral("seven_day"), QStringLiteral("Weekly · all models")}
            : minutes % 1440 == 0 ? std::pair{QStringLiteral("window_%1").arg(minutes),
                                              QStringLiteral("%1 days").arg(minutes / 1440)}
                                  : std::pair{QStringLiteral("window_%1").arg(minutes),
                                              QStringLiteral("%1 hours").arg((minutes + 59) / 60)};
        limits.append(QJsonObject{{QStringLiteral("id"), id},
                                  {QStringLiteral("label"), label},
                                  {QStringLiteral("usedPercent"), window.value("usedPercent")},
                                  {QStringLiteral("resetsAt"), window.value("resetsAt")}});
    }
    return limits;
}

} // namespace ketplus::daemon
