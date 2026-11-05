#include "runtime/SessionHost.h"

#include "providers/ClaudeAdapter.h"

#include <QSet>

#include <algorithm>

namespace ketplus::daemon {
namespace {

// A snapshot is display metadata. Bound it so a provider cannot grow every journal read.
constexpr qsizetype maximumTasks = 16;
constexpr qsizetype maximumTaskIdLength = 128;
constexpr qsizetype maximumTaskOutputBytes = 8 * 1024;

QJsonObject taskError(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QString bounded(const QJsonValue& value, const qsizetype maximum) {
    const QString text = value.toString();
    return text.size() <= maximum ? text : text.left(maximum - 1) + QChar(0x2026);
}

bool liveStatus(const QString& status) {
    return status == QStringLiteral("running") || status == QStringLiteral("stopping");
}

QJsonArray boundedTasks(const QJsonArray& source) {
    static const QSet<QString> kinds{QStringLiteral("shell"), QStringLiteral("agent"),
                                     QStringLiteral("other")};
    static const QSet<QString> statuses{QStringLiteral("running"), QStringLiteral("stopping"),
                                        QStringLiteral("completed"), QStringLiteral("failed"),
                                        QStringLiteral("stopped")};
    QJsonArray tasks;
    for (const QJsonValue& value : source) {
        if (tasks.size() >= maximumTasks)
            break;
        const QJsonObject task = value.toObject();
        const QString id = task.value(QStringLiteral("task_id")).toString();
        const QString status = task.value(QStringLiteral("status")).toString();
        if (id.isEmpty() || id.size() > maximumTaskIdLength || !statuses.contains(status))
            continue;
        const QString kind = task.value(QStringLiteral("kind")).toString();
        QJsonObject bounded_{
            {QStringLiteral("task_id"), id},
            {QStringLiteral("kind"), kinds.contains(kind) ? kind : QStringLiteral("other")},
            {QStringLiteral("title"), bounded(task.value(QStringLiteral("title")), 256)},
            {QStringLiteral("status"), status},
            {QStringLiteral("has_output"), task.value(QStringLiteral("has_output")).toBool()}};
        for (const auto& [key, maximum] :
             {std::pair{QStringLiteral("command"), qsizetype{1024}},
              std::pair{QStringLiteral("subagent_type"), qsizetype{64}},
              std::pair{QStringLiteral("summary"), qsizetype{512}},
              std::pair{QStringLiteral("last_tool"), qsizetype{128}},
              std::pair{QStringLiteral("result"), qsizetype{512}},
              std::pair{QStringLiteral("error"), qsizetype{512}},
              std::pair{QStringLiteral("reason"), qsizetype{64}}}) {
            if (task.value(key).isString() && !task.value(key).toString().isEmpty())
                bounded_.insert(key, bounded(task.value(key), maximum));
        }
        for (const QString& key :
             {QStringLiteral("started_at"), QStringLiteral("ended_at"), QStringLiteral("tool_uses"),
              QStringLiteral("total_tokens"), QStringLiteral("duration_ms")}) {
            if (task.value(key).isDouble() && task.value(key).toDouble() >= 0)
                bounded_.insert(key, task.value(key).toInteger());
        }
        tasks.append(bounded_);
    }
    return tasks;
}

bool hasLiveTask(const QJsonArray& tasks) {
    return std::any_of(tasks.constBegin(), tasks.constEnd(), [](const QJsonValue& value) {
        return liveStatus(value.toObject().value(QStringLiteral("status")).toString());
    });
}

} // namespace

bool SessionHost::isBackgroundTaskCommand(const QString& type) {
    return type == QStringLiteral("session.task.stop") ||
           type == QStringLiteral("session.task.output");
}

bool SessionHost::hasLiveBackgroundTasks(const Session& session) {
    return hasLiveTask(session.backgroundTasks);
}

void SessionHost::recordBackgroundTasks(Session& session, const QJsonArray& source) {
    const QJsonArray tasks = boundedTasks(source);
    if (tasks == session.backgroundTasks)
        return;
    session.backgroundTasks = tasks;
    if (!journal_->storeBackgroundTasks(session.stored.id, session.stored.generation, tasks))
        qWarning("Unable to store background tasks of %s: %s", qUtf8Printable(session.stored.id),
                 qUtf8Printable(journal_->errorString()));
    (void)appendEvent(session, QStringLiteral("tasks.changed"), {{QStringLiteral("tasks"), tasks}});
}

void SessionHost::endBackgroundTasks(Session& session, const QString& reason) {
    if (!hasLiveTask(session.backgroundTasks))
        return;
    QJsonArray tasks;
    for (const QJsonValue& value : std::as_const(session.backgroundTasks)) {
        QJsonObject task = value.toObject();
        if (liveStatus(task.value(QStringLiteral("status")).toString())) {
            task.insert(QStringLiteral("status"), QStringLiteral("stopped"));
            task.insert(QStringLiteral("reason"), reason);
        }
        tasks.append(task);
    }
    recordBackgroundTasks(session, tasks);
}

void SessionHost::backgroundTaskCommand(const QJsonObject& request, CommandCallback done) {
    Session* session = nullptr;
    const QJsonObject failure = checkedSession(request, &session);
    if (!failure.isEmpty()) {
        done(failure);
        return;
    }
    ProviderAdapter* const claude = provider(QStringLiteral("claude"));
    if (session->stored.adapter != QStringLiteral("claude") || claude == nullptr) {
        done(taskError(QStringLiteral("unsupported_provider"),
                       QStringLiteral("Only Claude conversations report background tasks.")));
        return;
    }
    const QString taskId = request.value(QStringLiteral("payload"))
                               .toObject()
                               .value(QStringLiteral("task_id"))
                               .toString();
    const auto listed = std::find_if(
        session->backgroundTasks.constBegin(), session->backgroundTasks.constEnd(),
        [&taskId](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("task_id")).toString() == taskId;
        });
    if (taskId.isEmpty() || listed == session->backgroundTasks.constEnd()) {
        done(taskError(QStringLiteral("task_not_found"),
                       QStringLiteral("This background task is no longer reported.")));
        return;
    }
    const bool stop =
        request.value(QStringLiteral("message_type")).toString() == QStringLiteral("session.task.stop");
    if (stop && !liveStatus((*listed).toObject().value(QStringLiteral("status")).toString())) {
        done(taskError(QStringLiteral("task_not_running"),
                       QStringLiteral("This background task has already ended.")));
        return;
    }
    claude->call(
        stop ? QStringLiteral("task/stop") : QStringLiteral("task/output"),
        {{QStringLiteral("sessionId"), session->stored.id}, {QStringLiteral("taskId"), taskId}},
        [stop, done = std::move(done)](const QJsonObject& result, const QString& error) {
            if (!error.isEmpty()) {
                done(taskError(QStringLiteral("task_unavailable"), error));
                return;
            }
            if (stop) {
                // The provider's next tasks.changed reports the stop.
                done({{QStringLiteral("ok"), true}});
                return;
            }
            QByteArray output = result.value(QStringLiteral("output")).toString().toUtf8();
            const bool cut = output.size() > maximumTaskOutputBytes;
            if (cut)
                output = output.right(maximumTaskOutputBytes);
            done({{QStringLiteral("ok"), true},
                  {QStringLiteral("output"), QString::fromUtf8(output)},
                  {QStringLiteral("total_bytes"),
                   result.value(QStringLiteral("total_bytes")).toInteger()},
                  {QStringLiteral("truncated"),
                   cut || result.value(QStringLiteral("truncated")).toBool()}});
        });
}

} // namespace ketplus::daemon
