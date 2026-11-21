#include "FakeRelay.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

// The Relay's job routes: acquire, then renew, start, finish and release on the lease.
namespace runner_app_test {

namespace {

using Reply = FakeRelay::Reply;

QString utc(const QDateTime& time) { return time.toUTC().toString(Qt::ISODate); }

Reply refused(const QString& code, QJsonObject extra = {}) {
    extra.insert(QStringLiteral("code"), code);
    extra.insert(QStringLiteral("message"), code);
    return {409, extra};
}

} // namespace

std::optional<FakeRelay::Reply> FakeRelay::handleJobs(const QString& runnerId,
                                                      const Request& request) {
    if (request.method != "POST")
        return std::nullopt;
    if (request.path == QStringLiteral("/v2/runner/jobs:acquire"))
        return acquire(runnerId, request.body);
    static const QRegularExpression leasePath(
        QStringLiteral("^/v2/runner/leases/([A-Za-z0-9_-]+)/(renew|start|finish|release)$"));
    const QRegularExpressionMatch match = leasePath.match(request.path);
    if (!match.hasMatch())
        return std::nullopt;
    return onLease(runnerId, match.captured(1), match.captured(2), request.body);
}

FakeRelay::Job* FakeRelay::jobOf(const QString& jobId) {
    for (Job& job : jobs)
        if (job.id == jobId)
            return &job;
    return nullptr;
}

QJsonObject FakeRelay::leaseJson(const Lease& lease) const {
    const auto job =
        std::find_if(jobs.cbegin(), jobs.cend(), [&](const Job& j) { return j.id == lease.jobId; });
    return {
        {QStringLiteral("jobId"), lease.jobId},
        {QStringLiteral("attempt"), job->attempt},
        {QStringLiteral("leaseId"), lease.id},
        {QStringLiteral("leaseExpiresAt"), utc(lease.expiresAt)},
        {QStringLiteral("requester"), QJsonObject{{QStringLiteral("workspaceId"), job->workspaceId},
                                                  {QStringLiteral("machineId"), job->machineId}}},
        {QStringLiteral("runsOn"), QJsonArray::fromStringList(job->runsOn)}};
}

FakeRelay::Reply FakeRelay::acquire(const QString& runnerId, const QJsonObject& body) {
    const QString acquireId = body.value(QStringLiteral("acquireId")).toString();
    if (acquireId.isEmpty())
        return {422, {{QStringLiteral("code"), QStringLiteral("invalid_request")}}};
    for (const Lease& lease : leases) {
        if (lease.runnerId != runnerId || !lease.open())
            continue;
        if (lease.acquireId == acquireId)
            return {200, leaseJson(lease)};
        QJsonObject held = leaseJson(lease);
        held.remove(QStringLiteral("requester"));
        held.remove(QStringLiteral("runsOn"));
        return refused(QStringLiteral("runner_busy"), {{QStringLiteral("lease"), held}});
    }
    if (disabled)
        return {403, {{QStringLiteral("code"), QStringLiteral("runner_disabled")}}};
    const QString wanted = body.value(QStringLiteral("jobId")).toString();
    Job* job = nullptr;
    for (Job& candidate : jobs)
        if (candidate.state == QStringLiteral("queued") &&
            (wanted.isEmpty() || candidate.id == wanted)) {
            job = &candidate;
            break;
        }
    if (job == nullptr)
        return refused(QStringLiteral("job_unavailable"));
    job->state = QStringLiteral("acquired");
    ++job->attempt;
    Lease lease;
    lease.id = QStringLiteral("lease-%1").arg(leases.size() + 1);
    lease.jobId = job->id;
    lease.runnerId = runnerId;
    lease.acquireId = acquireId;
    lease.expiresAt = m_now().addSecs(300);
    leases.insert(lease.id, lease);
    return {200, leaseJson(lease)};
}

FakeRelay::Reply FakeRelay::onLease(const QString& runnerId, const QString& leaseId,
                                    const QString& action, const QJsonObject& body) {
    if (!leases.contains(leaseId) || leases[leaseId].runnerId != runnerId)
        return refused(QStringLiteral("lease_expired"));
    Lease& lease = leases[leaseId];
    Job& job = *jobOf(lease.jobId);
    const QJsonObject jobJson{
        {QStringLiteral("jobId"), job.id},
        {QStringLiteral("state"), job.state},
        {QStringLiteral("reason"), job.reason.isEmpty() ? QJsonValue() : QJsonValue(job.reason)}};
    const bool endedByRelay = !job.reason.isEmpty();
    if (action == QStringLiteral("renew") || action == QStringLiteral("start")) {
        if (!lease.open())
            return refused(QStringLiteral("lease_expired"));
        if (endedByRelay)
            return refused(QStringLiteral("job_cancelled"), {{QStringLiteral("job"), jobJson}});
        lease.expiresAt = m_now().addSecs(300);
        if (action == QStringLiteral("renew"))
            return {200, {{QStringLiteral("leaseExpiresAt"), utc(lease.expiresAt)}}};
        if (lease.startedAt.isEmpty()) {
            lease.startedAt = utc(m_now());
            job.state = QStringLiteral("started");
        }
        return {200, {{QStringLiteral("startedAt"), lease.startedAt}}};
    }
    const bool release = action == QStringLiteral("release");
    const QString outcome =
        release ? QStringLiteral("released") : body.value(QStringLiteral("outcome")).toString();
    const QString reason = body.value(QStringLiteral("reason")).toString();
    const QJsonObject recorded{{QStringLiteral("outcome"), lease.recordedOutcome},
                               {QStringLiteral("reason"), lease.recordedReason}};
    if (!lease.open()) {
        if (lease.recordedOutcome != outcome || lease.recordedReason != reason)
            return refused(QStringLiteral("lease_closed"),
                           {{QStringLiteral("recorded"), recorded}});
    } else {
        if (endedByRelay)
            return refused(QStringLiteral("lease_closed"), {{QStringLiteral("job"), jobJson}});
        if (release && !lease.startedAt.isEmpty())
            return refused(QStringLiteral("job_started"));
        lease.recordedOutcome = outcome;
        lease.recordedReason = reason;
        if (release) {
            lease.requeued = job.attempt < 3;
            job.state = lease.requeued ? QStringLiteral("queued") : QStringLiteral("failed");
        } else {
            job.state = QStringLiteral("finished");
        }
    }
    if (release)
        return {200, {{QStringLiteral("requeued"), lease.requeued}}};
    return {200,
            {{QStringLiteral("jobId"), job.id},
             {QStringLiteral("state"), job.state},
             {QStringLiteral("outcome"), outcome},
             {QStringLiteral("reason"), reason},
             {QStringLiteral("endedAt"), utc(m_now())}}};
}

} // namespace runner_app_test
