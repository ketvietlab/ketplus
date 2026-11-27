#include "runtime/BackgroundTaskSummary.h"

#include <QSet>

namespace ketplus::daemon {

QJsonObject backgroundTaskSummary(const QList<QJsonObject>& managed, const QJsonArray& legacy) {
    static const QSet<QString> live{QStringLiteral("queued"), QStringLiteral("starting"),
                                    QStringLiteral("running"), QStringLiteral("stopping")};
    int count = 0;
    int queued = 0;
    QString title;
    const auto add = [&](const QJsonObject& task, const QString& state) {
        if (!live.contains(state))
            return;
        if (state == QStringLiteral("stopping") &&
            task.value(QStringLiteral("terminal_target")).toString() == QStringLiteral("cancelled"))
            return;
        ++count;
        queued += state == QStringLiteral("queued");
        if (title.isEmpty())
            title = task.value(QStringLiteral("title")).toString();
    };
    for (const QJsonObject& task : managed)
        add(task, task.value(QStringLiteral("state")).toString());
    for (const QJsonValue& value : legacy) {
        const QJsonObject task = value.toObject();
        add(task, task.value(QStringLiteral("status")).toString());
    }
    if (count == 0)
        return {{QStringLiteral("live_jobs"), 0}};
    return {{QStringLiteral("live_jobs"), count},
            {QStringLiteral("queued_jobs"), queued},
            {QStringLiteral("job_title"), title}};
}

} // namespace ketplus::daemon
