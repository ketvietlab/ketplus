#include "runner/remote/RelayCancels.h"

#include "runner/jobs/RemoteJobRecord.h"

#include <QLoggingCategory>

#include <algorithm>

namespace ketplus::daemon::runner::remote {

namespace {
Q_LOGGING_CATEGORY(lcCancels, "ketplus.runner.remote")
const QString userReason = QStringLiteral("user");
const QString jobNotFound = QStringLiteral("job_not_found");
} // namespace

RelayCancels::RelayCancels(jobs::JobRelay& relay, Schedule schedule)
    : relay_(relay), schedule_(std::move(schedule)) {}

RelayCancels::~RelayCancels() = default;

void RelayCancels::cancel(const QString& taskId, const QString& jobId, const QStringList& runsOn) {
    if (taskId.isEmpty() || cancels_.contains(taskId))
        return;
    cancels_.insert(taskId, Cancel{jobId, runsOn});
    attempt(taskId);
}

void RelayCancels::attempt(const QString& taskId) {
    const auto found = cancels_.find(taskId);
    if (found == cancels_.end())
        return;
    const quint64 generation = ++found->generation;
    const std::weak_ptr<int> alive = alive_;
    if (found->jobId.isEmpty()) {
        relay_.submit(taskId, found->runsOn,
                      [this, alive, taskId, generation](const jobs::JobOutcome& outcome) {
                          if (!alive.expired())
                              named(taskId, generation, outcome);
                      });
        return;
    }
    relay_.cancel(found->jobId, userReason,
                  [this, alive, taskId, generation](const jobs::JobOutcome& outcome) {
                      if (!alive.expired())
                          answered(taskId, generation, outcome);
                  });
}

void RelayCancels::named(const QString& taskId, const quint64 generation,
                         const jobs::JobOutcome& outcome) {
    const auto found = cancels_.find(taskId);
    if (found == cancels_.end() || found->generation != generation)
        return;
    if (!outcome.ok()) {
        // A refusal that ends a submission means the Relay holds no job to cancel.
        if (jobs::submitRefusalReason(outcome.failure.code)) {
            cancels_.erase(found);
            return;
        }
        retry(taskId);
        return;
    }
    found->jobId = outcome.value->jobId;
    found->retryMs = firstRetryMs;
    attempt(taskId);
}

void RelayCancels::answered(const QString& taskId, const quint64 generation,
                            const jobs::JobOutcome& outcome) {
    const auto found = cancels_.find(taskId);
    if (found == cancels_.end() || found->generation != generation)
        return;
    if (outcome.ok() || outcome.failure.code == jobNotFound) {
        qCInfo(lcCancels).noquote() << "relay cancel answered task=" + taskId;
        cancels_.erase(found);
        return;
    }
    // A refusal that asking again cannot change ends the cancel; the others are retried.
    if (jobs::submitRefusalReason(outcome.failure.code)) {
        qCWarning(lcCancels).noquote()
            << "relay cancel refused task=" + taskId + " code=" + outcome.failure.code;
        cancels_.erase(found);
        return;
    }
    retry(taskId);
}

void RelayCancels::retry(const QString& taskId) {
    const auto found = cancels_.find(taskId);
    if (found == cancels_.end())
        return;
    const qint64 delay = found->retryMs;
    found->retryMs = std::min(delay * 2, longestRetryMs);
    const quint64 generation = found->generation;
    const std::weak_ptr<int> alive = alive_;
    schedule_(delay, [this, alive, taskId, generation] {
        if (alive.expired())
            return;
        const auto current = cancels_.constFind(taskId);
        if (current != cancels_.cend() && current->generation == generation)
            attempt(taskId);
    });
}

} // namespace ketplus::daemon::runner::remote
