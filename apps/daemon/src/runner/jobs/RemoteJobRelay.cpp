#include "runner/jobs/RemoteJobRelay.h"

#include "runner/jobs/RemoteJobLabels.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ketplus::daemon::runner::jobs {
namespace {

void fail(const JobCallback& callback, const QString& code) {
    callback(JobOutcome{std::nullopt, runnerFailure(code)});
}

// Answers the parsed job when the reply is `expected` and names `jobId` (any id when empty).
void answer(const JobCallback& callback, const RelayReply& reply, const int expected,
            const QSet<QString>& codes, const QString& jobId) {
    if (auto failure = refusal(reply, expected, codes)) {
        callback(JobOutcome{std::nullopt, std::move(*failure)});
        return;
    }
    auto job = parseRelayJob(reply.body);
    if (!job || (!jobId.isEmpty() && job->jobId != jobId)) {
        fail(callback, QStringLiteral("invalid_response"));
        return;
    }
    callback(JobOutcome{std::move(job), {}});
}

QString jobPath(const QString& jobId) {
    return QStringLiteral("/v2/runner-jobs/") + jobId; // isJobId: nothing to escape
}

} // namespace

void RemoteJobRelay::submit(const QString& submitKey, const QStringList& runsOn,
                            JobCallback callback) {
    if (!isSubmitKey(submitKey)) {
        fail(callback, QStringLiteral("invalid_request"));
        return;
    }
    if (!isValidRunsOn(runsOn)) {
        fail(callback, QStringLiteral("invalid_labels"));
        return;
    }
    const QJsonObject body{{QStringLiteral("submitKey"), submitKey},
                           {QStringLiteral("runsOn"), QJsonArray::fromStringList(runsOn)}};
    port_.runnerRequest("POST", QStringLiteral("/v2/runner-jobs"), body,
                        [callback = std::move(callback)](const RelayReply& reply) {
                            answer(callback, reply, 202,
                                   {QStringLiteral("idempotency_conflict"),
                                    QStringLiteral("no_matching_runner"),
                                    QStringLiteral("queue_full"), QStringLiteral("invalid_labels")},
                                   {});
                        });
}

void RemoteJobRelay::get(const QString& jobId, JobCallback callback) {
    if (!isJobId(jobId)) {
        fail(callback, QStringLiteral("job_not_found"));
        return;
    }
    port_.runnerRequest("GET", jobPath(jobId), {},
                        [jobId, callback = std::move(callback)](const RelayReply& reply) {
                            answer(callback, reply, 200, {QStringLiteral("job_not_found")}, jobId);
                        });
}

void RemoteJobRelay::cancel(const QString& jobId, const QString& reason, JobCallback callback) {
    if (!isJobId(jobId)) {
        fail(callback, QStringLiteral("job_not_found"));
        return;
    }
    if (!isReasonCode(reason)) {
        fail(callback, QStringLiteral("invalid_request"));
        return;
    }
    port_.runnerRequest("POST", jobPath(jobId) + QStringLiteral("/cancel"),
                        QJsonObject{{QStringLiteral("reason"), reason}},
                        [jobId, callback = std::move(callback)](const RelayReply& reply) {
                            answer(callback, reply, 200, {QStringLiteral("job_not_found")}, jobId);
                        });
}

} // namespace ketplus::daemon::runner::jobs
