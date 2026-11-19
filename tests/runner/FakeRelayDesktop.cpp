#include "FakeRelay.h"

#include "pairing/PairingSignatures.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

// The desktop's routes: pairing tickets and approval, and job submit, read and cancel.
namespace runner_app_test {

namespace {

using Reply = FakeRelay::Reply;

Reply refusal(const int status, const QString& code) {
    return {status, {{QStringLiteral("code"), code}, {QStringLiteral("message"), code}}};
}

QJsonValue time(const QDateTime& value) {
    return value.isValid() ? QJsonValue(ketplus::runner::pairing::issuedAtString(value))
                           : QJsonValue();
}

QJsonValue text(const QString& value) { return value.isEmpty() ? QJsonValue() : value; }

bool terminal(const QString& state) {
    return QStringList{QStringLiteral("finished"), QStringLiteral("abandoned"),
                       QStringLiteral("expired"), QStringLiteral("cancelled"),
                       QStringLiteral("failed")}
        .contains(state);
}

const QRegularExpression& desktopRoute() {
    static const QRegularExpression route(
        QStringLiteral("^/v2/(runner-pairing/|runner-jobs(/|$)|runners(\\?|$)|runners/[^/:]+/"
                       "(unpair|disable|enable|remove)$|commands:poll(\\?|$)|events:append$)"));
    return route;
}

} // namespace

int FakeRelay::jobCalls(const QString& method, const QString& pathPart) const {
    return int(std::count_if(requests.cbegin(), requests.cend(), [&](const Request& request) {
        return request.method == method.toLatin1() &&
               request.path.startsWith(QStringLiteral("/v2/runner-jobs")) &&
               request.path.contains(pathPart);
    }));
}

QJsonObject FakeRelay::jobJson(const Job& job) const {
    return {{QStringLiteral("jobId"), job.id},
            {QStringLiteral("state"), job.state},
            {QStringLiteral("attempt"), job.attempt},
            {QStringLiteral("runnerId"), text(job.runnerId)},
            {QStringLiteral("queuedAt"), time(job.queuedAt)},
            {QStringLiteral("expiresAt"), time(job.queuedAt.addSecs(24 * 60 * 60))},
            {QStringLiteral("startedAt"), time(job.startedAt)},
            {QStringLiteral("endedAt"), time(job.endedAt)},
            {QStringLiteral("outcome"), text(job.outcome)},
            {QStringLiteral("reason"), text(job.reason)}};
}

bool FakeRelay::activePair(const QString& runnerId, const QString& machine) const {
    return std::any_of(tickets.cbegin(), tickets.cend(), [&](const Ticket& ticket) {
        return ticket.runnerId == runnerId && ticket.machineId == machine &&
               ticket.state == QStringLiteral("active");
    });
}

bool FakeRelay::runnerMatches(const QString& runnerId, const Job& job) const {
    const Runner runner = runners.value(runnerId);
    if (runner.id.isEmpty() || runner.removed || !activePair(runnerId, job.machineId))
        return false;
    QStringList labels = runner.labels;
    labels << QStringLiteral("os:linux") << QStringLiteral("arch:") + runner.arch;
    return std::all_of(job.runsOn.cbegin(), job.runsOn.cend(),
                       [&](const QString& label) { return labels.contains(label); });
}

void FakeRelay::jobChanged(const Job& job) {
    if (job.machineId != machineId)
        return;
    sendDesktop({{QStringLiteral("type"), QStringLiteral("job.changed")},
                 {QStringLiteral("jobId"), job.id},
                 {QStringLiteral("state"), job.state},
                 {QStringLiteral("attempt"), job.attempt},
                 {QStringLiteral("runnerId"), text(job.runnerId)},
                 {QStringLiteral("outcome"), text(job.outcome)},
                 {QStringLiteral("reason"), text(job.reason)}});
}

void FakeRelay::announce(const Job& job) {
    for (auto it = runners.cbegin(); it != runners.cend(); ++it)
        if (runnerMatches(it.key(), job))
            sendRunner(it.key(), {{QStringLiteral("type"), QStringLiteral("job.available")},
                                  {QStringLiteral("jobId"), job.id}});
}

std::optional<FakeRelay::Reply> FakeRelay::handleDesktop(const Request& request) {
    if (machineToken.isEmpty() || !desktopRoute().match(request.path).hasMatch())
        return std::nullopt;
    if (request.authorization != "Bearer " + machineToken.toLatin1())
        return refusal(401, QStringLiteral("invalid_access"));
    static const QRegularExpression ticketPath(
        QStringLiteral("^/v2/runner-pairing/tickets/([A-Za-z0-9_-]+)(/approve|/reject)?$"));
    static const QRegularExpression jobPath(
        QStringLiteral("^/v2/runner-jobs/([A-Za-z0-9_-]+)(/cancel)?$"));
    const QString& path = request.path;
    const bool post = request.method == "POST";
    // The daemon polls its commands whenever the socket wakes it; this Relay has none.
    if (!post && path.startsWith(QStringLiteral("/v2/commands:poll")))
        return Reply{200, {{QStringLiteral("commands"), QJsonArray()}}};
    // The daemon uploads its sessions' history; this Relay takes every event and keeps none.
    if (post && path == QStringLiteral("/v2/events:append"))
        return appendEvents(request.body);
    if (post && path == QStringLiteral("/v2/runner-pairing/tickets"))
        return createTicket(request.body);
    if (const auto match = ticketPath.match(path); match.hasMatch()) {
        const QString action = match.captured(2);
        if (!post && action.isEmpty())
            return ticketStatus(match.captured(1));
        if (post && action == QStringLiteral("/approve"))
            return approveTicket(match.captured(1), request.body);
        if (post && action == QStringLiteral("/reject")) {
            auto ticket = tickets.find(match.captured(1));
            if (ticket == tickets.end() || ticket->machineId != machineId)
                return refusal(404, QStringLiteral("ticket_not_found"));
            ticket->state = QStringLiteral("rejected");
            return Reply{204, {}};
        }
    }
    if (post && path == QStringLiteral("/v2/runner-jobs"))
        return submitJob(request.body);
    if (const auto match = jobPath.match(path); match.hasMatch()) {
        Job* job = jobOf(match.captured(1));
        if (!job || job->machineId != machineId)
            return refusal(404, QStringLiteral("job_not_found"));
        if (!post && match.captured(2).isEmpty())
            return Reply{200, jobJson(*job)};
        if (post && !match.captured(2).isEmpty())
            return cancelJob(job->id, request.body);
    }
    if (!post && path.startsWith(QStringLiteral("/v2/runners"))) {
        QJsonArray list;
        for (const Runner& runner : std::as_const(runners))
            list.append(QJsonObject{
                {QStringLiteral("runnerId"), runner.id},
                {QStringLiteral("name"), runner.name},
                {QStringLiteral("os"), QStringLiteral("linux")},
                {QStringLiteral("arch"), runner.arch},
                {QStringLiteral("labels"), QJsonArray::fromStringList(runner.labels)},
                {QStringLiteral("state"),
                 runner.removed ? QStringLiteral("removed") : QStringLiteral("active")},
                {QStringLiteral("runnerVersion"), QStringLiteral("0")},
                {QStringLiteral("lastSeenAt"), time(m_now())},
                {QStringLiteral("pairedWithThisMachine"), activePair(runner.id, machineId)}});
        return Reply{200, {{QStringLiteral("runners"), list}, {QStringLiteral("nextCursor"), {}}}};
    }
    violate(QStringLiteral("desktop route not served: %1 %2")
                .arg(QString::fromLatin1(request.method), path));
    return refusal(404, QStringLiteral("invalid_request"));
}

// Accepted through the highest cursor of each session generation the request carried.
FakeRelay::Reply FakeRelay::appendEvents(const QJsonObject& body) {
    QMap<QPair<QString, qint64>, qint64> highest;
    for (const QJsonValue& value : body.value(QStringLiteral("events")).toArray()) {
        const QJsonObject event = value.toObject();
        const QPair<QString, qint64> place{
            event.value(QStringLiteral("sessionId")).toString(),
            event.value(QStringLiteral("sessionGeneration")).toInteger()};
        highest[place] =
            qMax(highest.value(place), event.value(QStringLiteral("cursor")).toInteger());
    }
    QJsonArray accepted;
    for (auto it = highest.cbegin(); it != highest.cend(); ++it)
        accepted.append(QJsonObject{{QStringLiteral("sessionId"), it.key().first},
                                    {QStringLiteral("sessionGeneration"), it.key().second},
                                    {QStringLiteral("cursor"), it.value()}});
    return Reply{202, {{QStringLiteral("acceptedThrough"), accepted}}};
}

FakeRelay::Reply FakeRelay::createTicket(const QJsonObject& body) {
    const QJsonValue runnerId = body.value(QStringLiteral("runnerId"));
    if (runnerId.isString()) {
        if (!runners.contains(runnerId.toString()))
            return refusal(404, QStringLiteral("runner_not_found"));
        if (runners.value(runnerId.toString()).removed)
            return refusal(403, QStringLiteral("runner_removed"));
    }
    Ticket ticket;
    ticket.id = QStringLiteral("ticket-%1").arg(m_nextId++);
    ticket.secret = randomSecret();
    ticket.machineId = machineId;
    if (runnerId.isString())
        ticket.forRunner = runnerId.toString();
    tickets.insert(ticket.id, ticket);
    return {201,
            {{QStringLiteral("ticketId"), ticket.id},
             {QStringLiteral("ticketSecret"), ticket.secret},
             {QStringLiteral("expiresAt"), time(m_now().addSecs(60 * 60))}}};
}

FakeRelay::Reply FakeRelay::ticketStatus(const QString& ticketId) {
    const auto ticket = tickets.constFind(ticketId);
    if (ticket == tickets.cend() || ticket->machineId != machineId)
        return refusal(404, QStringLiteral("ticket_not_found"));
    QJsonObject body = ticket->state == QStringLiteral("claimed") ? ticket->claim : QJsonObject();
    body.insert(QStringLiteral("ticketId"), ticket->id);
    body.insert(QStringLiteral("pairState"), ticket->state);
    return {200, body};
}

// The Relay recomputes the digest from the stored keys; it does not verify the signature, which
// it stores and forwards unchanged.
FakeRelay::Reply FakeRelay::approveTicket(const QString& ticketId, const QJsonObject& body) {
    auto ticket = tickets.find(ticketId);
    if (ticket == tickets.end() || ticket->machineId != machineId)
        return refusal(404, QStringLiteral("ticket_not_found"));
    if (ticket->state == QStringLiteral("expired"))
        return refusal(410, QStringLiteral("ticket_expired"));
    if (ticket->state != QStringLiteral("claimed"))
        return refusal(409, QStringLiteral("ticket_used"));
    if (body.value(QStringLiteral("pairingDigest")).toString() != ticket->digest)
        return refusal(409, QStringLiteral("digest_mismatch"));
    ticket->state = QStringLiteral("active");
    ticket->approvalSignature = body.value(QStringLiteral("approvalSignature")).toString();
    sendRunner(ticket->runnerId,
               {{QStringLiteral("type"), QStringLiteral("runner.paired")},
                {QStringLiteral("ticketId"), ticket->id},
                {QStringLiteral("pairState"), ticket->state},
                {QStringLiteral("approvalSignature"), ticket->approvalSignature}});
    return {200, {{QStringLiteral("pairState"), ticket->state}}};
}

FakeRelay::Reply FakeRelay::submitJob(const QJsonObject& body) {
    const QString key = body.value(QStringLiteral("submitKey")).toString();
    QStringList runsOn;
    for (const QJsonValue& label : body.value(QStringLiteral("runsOn")).toArray())
        runsOn.append(label.toString());
    for (const Job& job : std::as_const(jobs)) {
        if (job.submitKey != key || job.machineId != machineId)
            continue;
        if (job.runsOn != runsOn)
            return refusal(409, QStringLiteral("idempotency_conflict"));
        return {202, jobJson(job)};
    }
    Job job;
    job.id = QStringLiteral("job_%1").arg(m_nextId++);
    job.submitKey = key;
    job.machineId = machineId;
    job.workspaceId = workspaceId;
    job.runsOn = runsOn;
    job.queuedAt = m_now();
    const bool matched = std::any_of(runners.cbegin(), runners.cend(), [&](const Runner& runner) {
        return runnerMatches(runner.id, job);
    });
    if (!matched)
        return refusal(409, QStringLiteral("no_matching_runner"));
    jobs.append(job);
    announce(job);
    return {202, jobJson(job)};
}

bool FakeRelay::expire(const QString& jobId) {
    Job* job = jobOf(jobId);
    if (job == nullptr || job->state != QStringLiteral("queued"))
        return false;
    job->state = QStringLiteral("expired");
    job->endedAt = m_now();
    jobChanged(*job);
    return true;
}

FakeRelay::Reply FakeRelay::cancelJob(const QString& jobId, const QJsonObject& body) {
    Job& job = *jobOf(jobId);
    if (terminal(job.state))
        return {200, jobJson(job)};
    job.state = QStringLiteral("cancelled");
    job.reason = body.value(QStringLiteral("reason")).toString();
    job.endedAt = m_now();
    for (const Lease& lease : std::as_const(leases))
        if (lease.jobId == jobId && lease.open())
            sendRunner(lease.runnerId, {{QStringLiteral("type"), QStringLiteral("job.cancel")},
                                        {QStringLiteral("jobId"), jobId},
                                        {QStringLiteral("leaseId"), lease.id}});
    jobChanged(job);
    return {200, jobJson(job)};
}

} // namespace runner_app_test
