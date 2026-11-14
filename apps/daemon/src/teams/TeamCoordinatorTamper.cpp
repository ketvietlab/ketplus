// ketplus#295 (ADR 0114 §2): a Codex reviewer reviews with Full access, and the broker compares
// the task's branch before and after each of its review turns. A change voids that review: the
// round is not counted, the task stays In review, its history and the leader say what changed,
// and nothing is reverted. Like a reply without a verdict, it is asked again a bounded number
// of times; then the leader decides.
#include "teams/TeamCoordinator.h"

#include "agents/ReviewIntegrity.h"

#include <QJsonArray>
#include <QJsonObject>

namespace ketplus::daemon::teams {

std::optional<review_integrity::Scope>
TeamCoordinator::reviewScope(const QString& reviewerSession, const QString& messageId) const {
    const Team* team = teamOf(reviewerSession);
    const ReviewSlot* slot = team ? team->reviewSlot(reviewerSession) : nullptr;
    if (!slot || slot->reviewing.isEmpty() || slot->message != messageId)
        return std::nullopt;
    const Task* task = team->task(slot->reviewing);
    if (!task)
        return std::nullopt;
    return review_integrity::Scope{.worktree = task->worktree,
                                   .branch = task->branch,
                                   .remoteDirectory = task->worktree.isEmpty()
                                                          ? taskScope(*team, *task).repository
                                                          : task->worktree,
                                   .taskHead = task->headCommit,
                                   .workingSession = task->owner};
}

void TeamCoordinator::reviewTampered(const QString& reviewerSession, const QString& messageId,
                                     const QJsonArray& changes) {
    Team* team = mutableTeamOf(reviewerSession);
    ReviewSlot* slot = team ? team->reviewSlot(reviewerSession) : nullptr;
    if (!slot || slot->reviewing.isEmpty() || slot->message != messageId)
        return;
    const Member* member = team->member(reviewerSession);
    if (!member || member->role != role::reviewer)
        return;
    const QString reviewerName = member->name;
    const Team before = *team;
    const QString taskId = slot->reviewing;
    slot->reviewing.clear();
    slot->message.clear();
    Task* task = team->task(taskId);
    QList<Update> updates;
    if (task && task->state == state::inReview) {
        const int round = task->round;
        const QString what = review_integrity::describe(changes);
        // Not counted, as a reply without a verdict: asked again, then the leader decides.
        --task->round;
        ++task->missingVerdicts;
        const bool askAgain = task->missingVerdicts <= verdictRetries;
        const QJsonObject data{{QStringLiteral("round"), round},
                               {QStringLiteral("reviewer"), reviewerName},
                               {QStringLiteral("changes"), changes}};
        const QString line =
            say(team->language,
                QT_TRANSLATE_NOOP("Team", "Round %1 does not count: %2 changed the branch "
                                          "during the review (%3). Nothing was reverted."))
                .arg(round)
                .arg(reviewerName, what);
        addHistory(*task, reviewerSession, QStringLiteral("review_tampered"), line, data);
        updates.append({.kind = QStringLiteral("review_tampered"),
                        .text = QStringLiteral("%1: %2").arg(taskId, line),
                        .taskId = taskId,
                        .actionable = true,
                        .data = data});
        if (askAgain) {
            slot->queue.removeAll(taskId);
            slot->queue.prepend(taskId);
        } else {
            // Back to In review once the leader has looked at the branch.
            task->resumeState = state::inReview;
            task->reviewBlock = QStringLiteral("review_tampered");
            task->question =
                say(team->language,
                    QT_TRANSLATE_NOOP("Team", "The review of round %1 changed the branch again, "
                                              "so it does not count. Check the branch, then "
                                              "hand the task to review again or hand it over."))
                    .arg(round);
            applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question,
                      updates);
        }
    }
    // commit() sends this reviewer's next review, or the same one asked again.
    (void)commit(*team, before, QStringLiteral("daemon"), updates);
}

} // namespace ketplus::daemon::teams
