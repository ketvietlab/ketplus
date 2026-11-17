// ketplus#295 (ADR 0114 §2): a Codex reviewer reviews with Full access, and the broker compares
// the task's branch before and after each of its review turns. A change voids that review: the
// round is not counted and the task is blocked for an explicit recovery decision.
// Missing source is an interruption, never proof that the reviewer edited it.
#include "teams/TeamCoordinator.h"

#include "agents/ReviewIntegrity.h"

#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>

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
        // Availability cannot establish tampering. Neither case is a content verdict,
        // and neither is automatically retried: fix the source or arbitrate first.
        task->round = std::max(0, task->round - 1);
        const bool interrupted = review_integrity::sourceUnavailable(changes);
        const QString outcome =
            interrupted ? QStringLiteral("review_interrupted") : QStringLiteral("review_tampered");
        const QJsonObject data{{QStringLiteral("round"), round},
                               {QStringLiteral("reviewer"), reviewerName},
                               {QStringLiteral("changes"), changes},
                               {QStringLiteral("content_round_counted"), false}};
        const QString line =
            interrupted
                ? say(team->language,
                      QT_TRANSLATE_NOOP("Team", "Round %1 interrupted: reviewed source is "
                                                "unavailable (%2). No reviewer edit is inferred."))
                      .arg(round)
                      .arg(what)
                : say(team->language,
                      QT_TRANSLATE_NOOP("Team", "Round %1 invalidated: readable source snapshots "
                                                "differ (%2). Nothing was reverted."))
                      .arg(round)
                      .arg(what);
        addHistory(*task, QStringLiteral("daemon"), outcome, line, data);
        updates.append({.kind = outcome,
                        .text = QStringLiteral("%1: %2").arg(taskId, line),
                        .taskId = taskId,
                        .actionable = true,
                        .data = data});
        task->resumeState = state::inReview;
        task->reviewBlock = outcome;
        task->question =
            interrupted
                ? say(team->language,
                      QT_TRANSLATE_NOOP("Team", "Restore and verify the reviewed source before "
                                                "explicitly retrying the review."))
                : say(team->language,
                      QT_TRANSLATE_NOOP("Team", "Inspect the source changes and explicitly request "
                                                "a new review; no verdict was accepted."));
        applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question, updates);
    }
    // commit() may send the next task, never an automatic retry of this invalid review.
    (void)commit(*team, before, QStringLiteral("daemon"), updates);
}

} // namespace ketplus::daemon::teams
