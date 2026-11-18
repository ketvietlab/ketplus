#include "runner/entry/RemoteJobEntry.h"

#include "runner/entry/RemoteSpec.h"
#include "runner/jobs/RemoteJobCoordinator.h"
#include "runner/remote/RemoteJobs.h"
#include "runner/sync/SnapshotPreparation.h"
#include "storage/BackgroundTaskLedger.h"

#include <QJsonArray>
#include <QLoggingCategory>

namespace ketplus::daemon::runner::entry {

namespace {

Q_LOGGING_CATEGORY(lcRemoteEntry, "ketplus.runner.entry")

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QString refusalMessage(const QString& code) {
    if (code == sync::prepare::gitUnsupported)
        return QStringLiteral("Git on this machine is too old for remote jobs.");
    return QStringLiteral("No runner is available to this machine.");
}

} // namespace

RemoteJobEntry::RemoteJobEntry(remote::RemoteJobs& jobs) : jobs_(jobs) {
    jobs_.setChanged([this](const jobs::RemoteJobRecord& record) {
        if (ports_.changed)
            ports_.changed(record.taskId);
    });
}

RemoteJobEntry::~RemoteJobEntry() { jobs_.setChanged({}); }

QJsonObject RemoteJobEntry::admit(const BackgroundTaskIntent& intent,
                                  const QJsonObject& execution) const {
    if (!intent.runsOn)
        return {};
    if (const QString problem = remoteSpecProblem(execution, intent.timeoutMs); !problem.isEmpty())
        return failure(QStringLiteral("invalid_request"), problem);
    if (const QString code = jobs_.refusal(); !code.isEmpty())
        return failure(code, refusalMessage(code));
    if (!sync::worktreeOf(execution.value(QStringLiteral("cwd")).toString()))
        return failure(sync::prepare::notAWorktree,
                       QStringLiteral("A remote job's cwd must be inside a Git worktree."));
    const std::optional<int> reserved = jobs_.store().unfinishedReserved();
    if (!reserved)
        return failure(QStringLiteral("storage_error"),
                       QStringLiteral("The remote jobs could not be counted."));
    if (jobs_.outstanding() + *reserved >= jobs::maximumOutstanding)
        return failure(QStringLiteral("resource_exhausted"),
                       QStringLiteral("64 remote jobs are already unfinished."));
    return {};
}

void RemoteJobEntry::submit(const QString& taskId, const QJsonObject& execution,
                            const qint64 timeoutMs, const QStringList& runsOn) {
    if (submitted_.contains(taskId))
        return;
    submitted_.insert(taskId);
    std::optional<protocol::JobSpec> spec = jobSpecOf(execution, timeoutMs);
    if (!spec) {
        if (ports_.failReserved)
            ports_.failReserved(taskId, QStringLiteral("invalid_request"));
        return;
    }
    remote::SubmitRequest request;
    request.spec = std::move(*spec);
    request.runsOn = runsOn;
    request.timeoutMs = timeoutMs;
    const std::weak_ptr<int> alive = alive_;
    jobs_.submit(taskId, std::move(request), [this, alive, taskId](const QString& code) {
        if (alive.expired() || code.isEmpty() || jobs_.record(taskId))
            return; // the record carries the task from here
        qCInfo(lcRemoteEntry).noquote()
            << QStringLiteral("remote job not submitted task=%1 code=%2").arg(taskId, code);
        if (ports_.failReserved)
            ports_.failReserved(taskId, code);
    });
}

std::optional<QString> RemoteJobEntry::stop(const QString& taskId) {
    if (!jobs_.record(taskId))
        return std::nullopt;
    return jobs_.stop(taskId).code;
}

std::optional<QString> RemoteJobEntry::resolveCleanup(const QString& taskId,
                                                      const QString& userClientId) {
    if (!jobs_.record(taskId))
        return std::nullopt;
    return jobs_.resolveCleanup(taskId, userClientId).code;
}

bool RemoteJobEntry::hasRecord(const QString& taskId) const {
    return jobs_.record(taskId).has_value();
}

QJsonObject RemoteJobEntry::placed(QJsonObject task) const {
    const auto record = jobs_.record(task.value(QStringLiteral("task_id")).toString());
    if (!record)
        return task;
    task.insert(QStringLiteral("placement"), record->placement);
    if (record->runner)
        task.insert(QStringLiteral("runner"),
                    QJsonObject{{QStringLiteral("runnerId"), record->runner->runnerId},
                                {QStringLiteral("name"), record->runner->name},
                                {QStringLiteral("attempt"), record->runner->attempt}});
    else
        task.insert(QStringLiteral("runner"), QJsonValue::Null);
    return task;
}

QJsonObject RemoteJobEntry::withPlacement(QJsonObject reply) const {
    if (reply.value(QStringLiteral("task")).isObject())
        reply.insert(QStringLiteral("task"),
                     placed(reply.value(QStringLiteral("task")).toObject()));
    if (reply.value(QStringLiteral("tasks")).isArray()) {
        QJsonArray tasks;
        for (const QJsonValue& task : reply.value(QStringLiteral("tasks")).toArray())
            tasks.append(task.isObject() ? QJsonValue(placed(task.toObject())) : task);
        reply.insert(QStringLiteral("tasks"), tasks);
    }
    return reply;
}

} // namespace ketplus::daemon::runner::entry
