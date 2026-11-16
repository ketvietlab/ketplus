#include "teams/TeamCoordinator.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace ketplus::daemon::teams {
namespace {
TeamResult refused(const QString& code, const QString& message) {
    return {.error = TeamError{code, message}};
}
bool reviewed(const Task& task) {
    if (task.round > 0 || !task.reviewBlock.isEmpty() || !task.reviewComment.isEmpty())
        return true;
    return std::any_of(task.history.cbegin(), task.history.cend(), [](const HistoryEntry& h) {
        return h.kind == QStringLiteral("verdict") || h.kind == QStringLiteral("review_accepted");
    });
}
} // namespace

TeamResult TeamCoordinator::decideRoute(const qint64 teamId, const QString& taskId,
                                        const QJsonObject& request, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return refused("team_not_found", "The team no longer exists.");
    if (actorRole(*team, actor) != role::leader)
        return refused("role_forbidden", "Only this team's leader records a risk route.");
    const QSet<QString> fields{"expected_team_revision",
                               "kind",
                               "route",
                               "head_commit",
                               "evidence_id",
                               "confidence",
                               "reason"};
    for (auto it = request.begin(); it != request.end(); ++it)
        if (!fields.contains(it.key()))
            return refused("invalid_payload", "Unknown routing field.");
    const auto expected = request.value("expected_team_revision");
    if (!expected.isDouble() || expected.toInteger(-1) != team->revision ||
        expected.toDouble() != static_cast<double>(team->revision))
        return refused("stale_team",
                       "Use the exact current team revision; decisions cannot be replayed.");
    static const QRegularExpression code(QStringLiteral("\\A[a-z][a-z0-9_]{0,99}\\z"));
    if (!code.match(request.value("reason").toString()).hasMatch())
        return refused("invalid_payload", "A bounded reason code is required, not tool output.");
    if (request.contains("route") && !request.value("route").isString())
        return refused("invalid_payload", "Route must be a string.");
    const QString wanted = request.value("route").toString(QStringLiteral("auto"));
    if (wanted != "auto" && wanted != "leader_direct" && wanted != "developer_leader" &&
        wanted != "developer_reviewer")
        return refused("invalid_payload", "Unknown risk route.");
    if (request.contains("confidence")) {
        const auto confidence = request.value("confidence");
        const int number = confidence.toInt(-1);
        if (!confidence.isDouble() || number < 0 || number > 100 || confidence.toDouble() != number)
            return refused("invalid_payload", "Confidence must be an integer percentage.");
    }
    const QString kind = request.value("kind").toString();
    risk::Decision decision;
    decision.teamId = teamId;
    decision.taskId = taskId;
    decision.kind = kind;
    decision.actor = actor;
    decision.teamRevision = team->revision + 1;
    decision.at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Task* task = nullptr;
    if (risk::advisoryKind(kind)) {
        if (!taskId.isEmpty() || request.contains("head_commit") ||
            request.contains("evidence_id") || request.contains("confidence") ||
            (wanted != "auto" && wanted != "leader_direct"))
            return refused("invalid_payload", "Advice has no task or change authorization.");
        decision.route = risk::Route::LeaderDirect;
        decision.risk = QStringLiteral("advisory");
        decision.gateReason = QStringLiteral("advisory_no_action");
        decision.revision = QStringLiteral("team:%1:%2").arg(teamId).arg(team->revision);
        decision.followUp = QStringLiteral("answer_only");
    } else if (kind == QStringLiteral("change")) {
        task = team->task(taskId);
        if (!task)
            return refused("task_not_found", "A change route requires an existing task.");
        if (task->state != state::inProgress || reviewed(*task))
            return refused("review_locked",
                           "Do not reclassify a reviewed task or one with findings.");
        const QString head = request.value("head_commit").toString();
        static const QRegularExpression commit(QStringLiteral("\\A[0-9a-f]{40}\\z"));
        const Team scope = taskScope(*team, *task);
        const QString liveHead = host_->taskCommit(scope, *task);
        const auto dirty = host_->hasUncommittedWork(task->worktree);
        if (!commit.match(head).hasMatch() || liveHead != head || !dirty || *dirty)
            return refused("stale_revision", "Use the exact readable, clean task revision.");
        decision.revision = head;
        decision.taskRevision = task->changedRevision;
        const QString id = request.value("evidence_id").toString();
        if (request.contains("evidence_id") && !risk::evidenceId(id))
            return refused("invalid_payload", "Evidence must be an opaque receipt ID.");
        const auto evidence =
            id.isEmpty() ? std::nullopt : host_->routingEvidence(*team, *task, id);
        if (evidence &&
            (evidence->id != id || evidence->teamId != teamId || evidence->taskId != taskId ||
             evidence->revision != head || evidence->taskRevision != task->changedRevision))
            return refused("stale_evidence", "Evidence belongs to another task or revision.");
        auto evaluation =
            risk::evaluate(evidence ? &*evidence : nullptr, request.value("confidence").toInt(-1));
        const Member* owner = team->member(task->owner);
        const bool complete =
            !task->criteria.isEmpty() && std::all_of(task->criteria.cbegin(), task->criteria.cend(),
                                                     [](const Criterion& c) { return c.met; });
        const bool dependencies = std::all_of(task->dependsOn.cbegin(), task->dependsOn.cend(),
                                              [team](const QString& id) {
                                                  const Task* d = team->task(id);
                                                  return d && d->state == state::done;
                                              });
        if (!complete || !dependencies ||
            scope.integrationBranch != QStringLiteral("integration") || !owner ||
            owner->role != role::developer || task->owner == actor) {
            evaluation = {risk::Route::DeveloperReviewer,
                          QStringLiteral("acceptance_or_scope_incomplete")};
        }
        if (wanted == QStringLiteral("leader_direct") ||
            (wanted == QStringLiteral("developer_leader") &&
             evaluation.route != risk::Route::DeveloperLeader))
            return refused("review_required", evaluation.reason);
        decision.gateReason = wanted == QStringLiteral("developer_reviewer") &&
                                      evaluation.route == risk::Route::DeveloperLeader
                                  ? QStringLiteral("conservative_review_requested")
                                  : evaluation.reason;
        decision.confidence = request.value("confidence").toInt(-1);
        decision.route = wanted == QStringLiteral("developer_reviewer")
                             ? risk::Route::DeveloperReviewer
                             : evaluation.route;
        decision.risk = decision.route == risk::Route::DeveloperLeader
                            ? QStringLiteral("low")
                            : QStringLiteral("high_or_unknown");
        decision.followUp = decision.route == risk::Route::DeveloperLeader
                                ? QStringLiteral("merge_then_acceptance")
                                : QStringLiteral("review_required");
        if (evidence) {
            decision.evidenceIds.append(id);
            for (const auto& check : evidence->checks)
                if (risk::evidenceId(check.evidenceId) &&
                    !decision.evidenceIds.contains(check.evidenceId))
                    decision.evidenceIds.append(check.evidenceId);
            decision.scopeEvidence =
                risk::evidenceId(evidence->scopeEvidence) ? evidence->scopeEvidence : QString{};
            decision.rollbackEvidence = risk::evidenceId(evidence->rollbackEvidence)
                                            ? evidence->rollbackEvidence
                                            : QString{};
        }
    } else
        return refused("invalid_payload", "Unknown work kind.");
    decision.reason = request.value("reason").toString();
    const Team before = *team;
    // Detach after the before snapshot: QList<Task> uses implicit sharing.
    if (!taskId.isEmpty())
        team->task(taskId)->routingDecision = decision;
    team->lastRoutingDecision = decision;
    const QJsonObject data = risk::toJson(decision);
    const Update update{.kind = QStringLiteral("risk_route"),
                        .text =
                            QStringLiteral("Risk route: %1").arg(risk::routeName(decision.route)),
                        .taskId = taskId,
                        .data = data};
    if (!taskId.isEmpty())
        addHistory(*team->task(taskId), actor, QStringLiteral("risk_route"), update.text, data);
    if (const auto error = commit(*team, before, actor, {update}))
        return {.error = error};
    return {.teamId = teamId};
}
} // namespace ketplus::daemon::teams
