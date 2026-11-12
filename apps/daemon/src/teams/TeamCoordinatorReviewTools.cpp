// T-19: the leader corrects the review record of a task. A verdict that should not have been
// counted is voided; a review made outside the board, for example on the pull request, is
// accepted. Both leave a task blocked by its review without a new commit.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

TeamResult refused(const qint64 teamId, const QString& code, const QString& message) {
    return {.error = TeamError{code, message}, .teamId = teamId};
}

// The newest verdict that counted a round, unless a void came after it.
const HistoryEntry* lastCountedVerdict(const Task& task) {
    for (qsizetype index = task.history.size() - 1; index >= 0; --index) {
        const HistoryEntry& entry = task.history.at(index);
        if (entry.kind == QStringLiteral("verdict_voided"))
            return nullptr;
        if (entry.kind != QStringLiteral("verdict"))
            continue;
        const QString verdict = entry.data.value(QStringLiteral("verdict")).toString();
        if (verdict == QStringLiteral("findings") || verdict == QStringLiteral("no_findings"))
            return &entry;
    }
    return nullptr;
}

void withdrawMerges(Team& team, const QString& taskId, const QString& reason) {
    for (Decision& decision : team.decisions)
        if (decision.kind == QStringLiteral("merge") && decision.taskId == taskId &&
            decision.state == QStringLiteral("open")) {
            decision.state = QStringLiteral("withdrawn");
            decision.reason = reason;
            decision.answeredAt = nowIso();
        }
}

} // namespace

bool TeamCoordinator::blockedByReview(const Task& task) {
    return task.state == state::blocked &&
           (task.question.startsWith(QStringLiteral("Review round limit reached (")) ||
            (task.question.startsWith(QStringLiteral("The review of round ")) &&
             task.question.contains(QStringLiteral(" has no verdict after "))));
}

TeamResult TeamCoordinator::voidReview(const qint64 teamId, const QString& taskId,
                                       const QString& reason, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return refused(teamId, QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists."));
    if (actorRole(*team, actor) != role::leader)
        return refused(teamId, QStringLiteral("role_forbidden"),
                       QStringLiteral("Only the team's leader voids a review round."));
    const Task* found = std::as_const(*team).task(taskId);
    if (!found)
        return refused(teamId, QStringLiteral("task_not_found"),
                       QStringLiteral("The team has no task %1.").arg(taskId));
    if (reason.trimmed().isEmpty())
        return refused(teamId, QStringLiteral("invalid_payload"),
                       QStringLiteral("Say why the round is voided."));
    if (found->state == state::done || found->state == state::inReview || team->reviewing == taskId)
        return refused(teamId, QStringLiteral("invalid_transition"),
                       QStringLiteral("%1 has no verdict to void: it is %2.")
                           .arg(taskId, found->state == state::done ? QStringLiteral("done")
                                                                    : QStringLiteral("in review")));
    const HistoryEntry* verdict = lastCountedVerdict(*found);
    const int round = verdict ? verdict->data.value(QStringLiteral("round")).toInt() : 0;
    if (!verdict || round != found->round || round < 1)
        return refused(teamId, QStringLiteral("invalid_transition"),
                       QStringLiteral("%1 has no recorded review round to void.").arg(taskId));
    const Team before = *team;
    Task* task = team->task(taskId);
    const QString was = verdict->data.value(QStringLiteral("verdict")).toString();
    const QString by = actorName(*team, actor);
    const QString why = reason.trimmed().left(2000);
    --task->round;
    task->missingVerdicts = 0;
    task->question.clear();
    task->resumeState.clear();
    addHistory(*task, actor, QStringLiteral("verdict_voided"),
               QStringLiteral("Round %1's verdict (%2) voided by %3: %4")
                   .arg(round)
                   .arg(was == QStringLiteral("no_findings") ? QStringLiteral("no findings")
                                                             : QStringLiteral("findings"),
                        by, why),
               {{QStringLiteral("round"), round},
                {QStringLiteral("verdict"), was},
                {QStringLiteral("by"), by},
                {QStringLiteral("reason"), why}});
    withdrawMerges(*team, taskId, QStringLiteral("The review of %1 was voided.").arg(taskId));
    QList<Update> updates;
    // Back to the state it had before the verdict; the review queue takes it again.
    applyMove(*team, *task, state::inReview, actor, QStringLiteral("round %1 voided").arg(round),
              updates);
    if (auto error = commit(*team, before, actor, updates))
        return {.error = error, .teamId = teamId};
    return {.error = std::nullopt, .teamId = teamId};
}

TeamResult TeamCoordinator::acceptReview(const qint64 teamId, const QString& taskId,
                                         const QString& evidence, const QString& reason,
                                         const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return refused(teamId, QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists."));
    if (actorRole(*team, actor) != role::leader)
        return refused(teamId, QStringLiteral("role_forbidden"),
                       QStringLiteral("Only the team's leader accepts a review."));
    const Task* found = std::as_const(*team).task(taskId);
    if (!found)
        return refused(teamId, QStringLiteral("task_not_found"),
                       QStringLiteral("The team has no task %1.").arg(taskId));
    if (found->state != state::inReview && !blockedByReview(*found))
        return refused(teamId, QStringLiteral("invalid_transition"),
                       QStringLiteral("%1 is %2: only a task in review, or blocked by its "
                                      "review, can have a review accepted.")
                           .arg(taskId, found->state));
    if (evidence.trimmed().isEmpty() || reason.trimmed().isEmpty())
        return refused(teamId, QStringLiteral("invalid_payload"),
                       QStringLiteral("Give the evidence of the review (for example the pull "
                                      "request review's URL) and the reason."));
    const Team before = *team;
    Task* task = team->task(taskId);
    const QString by = actorName(*team, actor);
    const QString proof = evidence.trimmed().left(2000);
    const QString why = reason.trimmed().left(2000);
    // A review of it that is queued or in flight is dropped; its answer is then ignored.
    team->reviewQueue.removeAll(taskId);
    const bool wasReviewing = team->reviewing == taskId;
    if (wasReviewing) {
        team->reviewing.clear();
        team->reviewingMessage.clear();
    }
    task->missingVerdicts = 0;
    task->question.clear();
    task->resumeState.clear();
    addHistory(*task, actor, QStringLiteral("review_accepted"),
               QStringLiteral("Review accepted by %1, made outside the board (%2): %3")
                   .arg(by, proof, why),
               {{QStringLiteral("by"), by},
                {QStringLiteral("evidence"), proof},
                {QStringLiteral("reason"), why},
                {QStringLiteral("round"), task->round}});
    QList<Update> updates;
    applyMove(*team, *task, state::ready, actor, QStringLiteral("review accepted by %1").arg(by),
              updates);
    offerMerges(*team, updates);
    if (auto error = commit(*team, before, actor, updates))
        return {.error = error, .teamId = teamId};
    if (wasReviewing)
        pumpReviews(teamId);
    return {.error = std::nullopt, .teamId = teamId};
}

} // namespace ketplus::daemon::teams
