#include "runtime/SessionHost.h"

#include <QCryptographicHash>
#include <QJsonDocument>

namespace ketplus::daemon {
namespace {
QJsonObject refused(const QString& code, const QString& reason) {
    return {{"ok", false}, {"error_code", code}, {"error_message", reason}};
}
QString key(const QString& session, const QJsonObject& payload) {
    return session + QLatin1Char(':') +
           QString::fromLatin1(
               QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                        QCryptographicHash::Sha256)
                   .toHex());
}
QJsonObject verifyRequest(const teams::Team* team, const QString& session,
                          const QJsonObject& payload) {
    if (!team)
        return refused("not_on_team", "This conversation is on no team.");
    const auto* member = team->member(session);
    const auto* task = team->task(payload.value("task_id").toString());
    if (!member || (member->role != teams::role::leader &&
                    (member->role != teams::role::developer || !task || task->owner != session)))
        return refused("role_forbidden", "Only the task's developer or same-team leader verifies.");
    const QSet<QString> fields{"task_id", "expected_team_revision", "head_commit"};
    for (auto it = payload.begin(); it != payload.end(); ++it)
        if (!fields.contains(it.key()))
            return refused("invalid_payload",
                           "Verification accepts identifiers, not pass/risk assertions.");
    if (!payload.value("task_id").isString() || !payload.value("head_commit").isString())
        return refused("invalid_payload", "Task and exact head identifiers are required.");
    const auto expected = payload.value("expected_team_revision");
    if (!expected.isDouble() || expected.toInteger(-1) != team->revision ||
        expected.toDouble() != static_cast<double>(team->revision))
        return refused("stale_team", "Read the current team revision before verifying.");
    if (!task)
        return refused("task_not_found", "The task no longer exists.");
    return {};
}
} // namespace

void SessionHost::prepareTeamRouting(const QString& session, const QJsonObject& payload,
                                     std::function<void()> then) {
    const auto* team = teams_ ? teams_->teamOf(session) : nullptr;
    if (!verifyRequest(team, session, payload).isEmpty()) {
        then();
        return;
    }
    if (routingVerifications_ >= 4) {
        routingReplies_.insert(
            key(session, payload),
            refused("verification_busy", "Wait for an existing verification to finish."));
        then();
        return;
    }
    ++routingVerifications_;
    const auto snapshot = *team;
    const auto task = *team->task(payload.value("task_id").toString());
    teamGit_.start([guard = QPointer<SessionHost>(this), snapshot, task, session, payload,
                    then = std::move(then)]() mutable {
        const auto result = teams::RoutingEvidence::produce(
            snapshot, task, payload.value("head_commit").toString());
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, snapshot, task, session, payload, result, then = std::move(then)]() mutable {
                if (!guard)
                    return;
                --guard->routingVerifications_;
                const auto* current = guard->teams_->teamOf(session);
                QJsonObject reply = verifyRequest(current, session, payload);
                const auto* live = current ? current->task(task.id) : nullptr;
                if (reply.isEmpty() && (current->id != snapshot.id || !live ||
                                        live->changedRevision != task.changedRevision))
                    reply = refused("stale_evidence", "The task changed while verification ran.");
                if (reply.isEmpty() && !result.evidence)
                    reply = refused(result.reason == "stale_revision" ? "stale_revision"
                                                                      : "review_required",
                                    result.reason);
                if (reply.isEmpty()) {
                    const auto& evidence = *result.evidence;
                    guard->routingEvidence_.retain(*current, *live, evidence);
                    QJsonArray checks;
                    for (const auto& check : evidence.checks)
                        checks.append(QJsonObject{{"name", check.name},
                                                  {"passed", check.passed},
                                                  {"revision", check.revision},
                                                  {"evidence_id", check.evidenceId}});
                    reply = {{"ok", true},
                             {"evidence_id", evidence.id},
                             {"recipe", result.recipe},
                             {"revision", evidence.revision},
                             {"task_revision", evidence.taskRevision},
                             {"checks", checks},
                             {"authorization_granted", false}};
                }
                if (guard->routingReplies_.size() >= 128)
                    guard->routingReplies_.erase(guard->routingReplies_.begin());
                guard->routingReplies_.insert(key(session, payload), reply);
                then();
            },
            Qt::QueuedConnection);
    });
}

std::optional<teams::risk::VerifiedEvidence> SessionHost::routingEvidence(const teams::Team& team,
                                                                          const teams::Task& task,
                                                                          const QString& id) const {
    return routingEvidence_.lookup(team, task, id);
}

QJsonObject SessionHost::teamRoutingOperation(const QString& session, const QString& operation,
                                              const QJsonObject& payload) {
    const auto* team = teams_ ? teams_->teamOf(session) : nullptr;
    if (operation == QStringLiteral("team_task_route_verify")) {
        const QJsonObject error = verifyRequest(team, session, payload);
        if (!error.isEmpty())
            return error;
        const auto reply = routingReplies_.take(key(session, payload));
        return reply.isEmpty()
                   ? refused("verification_required", "Use the deferred agent tool transport.")
                   : reply;
    }
    if (!team)
        return refused("not_on_team", "This conversation is on no team.");
    if (payload.contains("task_id") && !payload.value("task_id").isString())
        return refused("invalid_payload", "Task ID must be a string.");
    const qint64 teamId = team->id;
    const QString taskId = payload.value("task_id").toString();
    QJsonObject request = payload;
    request.remove("task_id");
    const auto result = teams_->decideRoute(teamId, taskId, request, session);
    if (!result.ok())
        return refused(result.error->code, result.error->message);
    const auto* current = teams_->team(teamId);
    const auto decision =
        taskId.isEmpty() ? current->lastRoutingDecision : current->task(taskId)->routingDecision;
    return {{"ok", true},
            {"routing_decision", teams::risk::toJson(*decision)},
            {"conclusion_source", "leader_policy"},
            {"reviewer_verdict", false},
            {"authorization_granted", false}};
}
} // namespace ketplus::daemon
