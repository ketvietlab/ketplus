#include "runner/jobs/RemoteJobSpecs.h"

namespace ketplus::daemon::runner::jobs {

namespace {
peer::protocol::JobSpec unbound(peer::protocol::JobSpec spec) {
    spec.epoch = 0;
    spec.jobId.clear();
    spec.attempt = 1;
    return spec;
}
} // namespace

bool RemoteJobSpecs::store(const QString& taskId, peer::protocol::JobSpec spec) {
    if (taskId.isEmpty() || ended_.contains(taskId))
        return false;
    spec = unbound(std::move(spec));
    const auto held = specs_.constFind(taskId);
    if (held != specs_.cend())
        return *held == spec;
    specs_.insert(taskId, spec);
    return true;
}

void RemoteJobSpecs::recorded(const RemoteJobRecord& record) {
    if (record.terminal()) {
        specs_.remove(record.taskId);
        ended_.insert(record.taskId);
        if (!record.jobId.isEmpty())
            tasks_.remove(record.jobId);
        return;
    }
    if (!record.jobId.isEmpty())
        tasks_.insert(record.jobId, record.taskId);
}

std::optional<peer::protocol::JobSpec> RemoteJobSpecs::spec(const QString& jobId,
                                                            const qint64 attempt) {
    const auto task = tasks_.constFind(jobId);
    if (jobId.isEmpty() || task == tasks_.cend())
        return std::nullopt;
    const auto held = specs_.constFind(*task);
    if (held == specs_.cend())
        return std::nullopt;
    peer::protocol::JobSpec bound = *held;
    bound.jobId = jobId;
    bound.attempt = attempt;
    return bound;
}

} // namespace ketplus::daemon::runner::jobs
