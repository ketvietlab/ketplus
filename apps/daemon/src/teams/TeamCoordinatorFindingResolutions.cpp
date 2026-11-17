// Leader arbitration is separate from a reviewer verdict and never completes a merge.
#include "teams/TeamCompletionPolicy.h"
#include "teams/TeamCoordinator.h"

#include <algorithm>
#include <utility>

namespace ketplus::daemon::teams {

TeamResult TeamCoordinator::acceptReview(const qint64 teamId, const QString& taskId,
                                         const QString& evidence, const QString& reason,
                                         const QString& actor) {
    const auto refused = [teamId](const QString& code, const QString& message) {
        return TeamResult{.error = TeamError{code, message}, .teamId = teamId};
    };
    Team* team = mutableTeam(teamId);
    if (!team)
        return refused("team_not_found", "The team no longer exists.");
    if (actorRole(*team, actor) != role::leader)
        return refused("role_forbidden", "Only this team's leader arbitrates findings.");
    const Task* found = std::as_const(*team).task(taskId);
    if (!found)
        return refused("task_not_found", "The team has no such task.");
    if ((found->state != state::inProgress || !found->reviewFindings) && !blockedByReview(*found))
        return refused("invalid_transition",
                       "Only finished findings or a review-blocked task can be arbitrated.");
    const auto request = review_policy::request(evidence);
    if (!request || !review_policy::code(reason))
        return refused("invalid_payload",
                       "Use the versioned review decision and a bounded reason code.");
    if (request->teamRevision != team->revision || request->taskRevision != found->changedRevision)
        return refused("stale_revision", "Refresh both team and task revisions before deciding.");
    if (found->state == state::done || found->state == state::ready ||
        team->reviewInFlight(taskId) || found->state == state::inReview)
        return refused("invalid_transition",
                       "Wait for the bound review to finish before arbitration.");
    if (found->reviewBlock == "review_tampered" || found->reviewBlock == "review_interrupted")
        return refused("review_required",
                       "Restore and verify the reviewed source, then explicitly retry review.");
    const Team scope = taskScope(*team, *found);
    const auto dirty = host_->hasUncommittedWork(found->worktree);
    if (host_->taskCommit(scope, *found) != request->head || !dirty || *dirty)
        return refused("stale_revision",
                       "The current task checkout must be readable, clean and at the exact head.");
    if (request->action == "request_review") {
        if (!blockedByReview(*found) || found->round < 1 || found->round >= 999999)
            return refused("invalid_transition",
                           "Only a review-blocked task can receive a further review pass.");
        const Team before = *team;
        Task* task = team->task(taskId);
        task->headCommit = request->head;
        task->arbitratedRound = task->round + 1;
        task->arbitrationHead = request->head;
        task->reviewBlock.clear();
        task->question.clear();
        task->resumeState.clear();
        addHistory(*task, actor, "review_arbitrated",
                   "Leader explicitly requested one further independent review.",
                   {{"action", "request_review"},
                    {"reason", reason},
                    {"actor", actor},
                    {"revision", request->head},
                    {"after_round", task->round},
                    {"authorized_round", task->arbitratedRound},
                    {"at", nowIso()}});
        QList<Update> updates;
        applyMove(*team, *task, state::inReview, actor, "explicit leader arbitration", updates);
        if (auto error = commit(*team, before, actor, updates))
            return {.error = error, .teamId = teamId};
        return {.teamId = teamId};
    }
    const Member* owner = team->member(found->owner);
    if (!found->reviewFindings || found->reviewFindings->legacy || found->owner == actor ||
        !owner || owner->role != role::developer)
        return refused("review_required",
                       "Legacy, unknown or self-owned findings require an independent reviewer.");
    const bool complete =
        !found->criteria.isEmpty() && std::all_of(found->criteria.cbegin(), found->criteria.cend(),
                                                  [](const Criterion& c) { return c.met; });
    const bool dependencies =
        std::all_of(found->dependsOn.cbegin(), found->dependsOn.cend(), [team](const QString& id) {
            const Task* t = team->task(id);
            return t && t->state == state::done;
        });
    const bool pending =
        std::any_of(team->decisions.cbegin(), team->decisions.cend(), [&](const Decision& d) {
            return d.state == "open" && (d.taskId.isEmpty() || d.taskId == taskId);
        });
    if (!complete || !dependencies || pending || scope.integrationBranch != "integration")
        return refused(
            "acceptance_incomplete",
            "Acceptance, dependencies, scope and required user decisions must be complete.");
    const auto receipt = host_->routingEvidence(*team, *found, request->receipt);
    if (!receipt || receipt->teamId != teamId || receipt->taskId != taskId)
        return refused("evidence_missing",
                       "A trusted host receipt for the reviewed baseline and fix is required.");
    const auto resolved =
        review_policy::resolve(*found->reviewFindings, *request, *receipt, actor, nowIso());
    if (!resolved)
        return refused(
            "review_required",
            "Findings, risk, required checks or receipt provenance require independent review.");
    const Team before = *team;
    Task* task = team->task(taskId);
    task->headCommit = request->head;
    task->reviewFindings = *resolved;
    task->missingVerdicts = 0;
    task->reviewBlock.clear();
    task->question.clear();
    task->resumeState.clear();
    addHistory(*task, actor, "findings_resolved",
               "Leader verified bounded findings; independent reviewer verdict unchanged.",
               {{"route", "leader_targeted_verification"},
                {"risk", "verified_low_risk"},
                {"confidence", request->confidence},
                {"task_revision", request->taskRevision},
                {"team_revision", request->teamRevision},
                {"reason", reason},
                {"actor", actor},
                {"revision", request->head},
                {"reviewed_revision", receipt->reviewedRevision},
                {"evidence", receipt->id},
                {"resolutions", review_policy::toJson(*resolved).value("findings")},
                {"at", nowIso()},
                {"follow_up", "merge_into_integration_after_acceptance"}});
    QList<Update> updates;
    applyMove(*team, *task, state::ready, actor, "bounded findings verified by leader", updates);
    completion::seal(*team, *task, "leader_targeted_verification", actor,
                     host_->completionRepository(*team, *task));
    offerMerges(*team, updates);
    if (auto error = commit(*team, before, actor, updates))
        return {.error = error, .teamId = teamId};
    return {.teamId = teamId};
}

} // namespace ketplus::daemon::teams
