// ADR 0114 §5 and open point 3: who merges a reviewed task, and how the daemon knows it is
// merged. The daemon never merges; it notices the integration branch containing the task's
// branch, the same way for a merge in the app and one made by hand.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

bool dependenciesDone(const Team& team, const Task& task) {
    return std::all_of(task.dependsOn.cbegin(), task.dependsOn.cend(), [&](const QString& id) {
        const Task* dependency = team.task(id);
        return dependency && dependency->state == state::done;
    });
}

} // namespace

void TeamCoordinator::offerMerges(Team& team, QList<Update>& updates) {
    for (Task& task : team.tasks) {
        if (task.state != state::ready || task.mergeOffered || !dependenciesDone(team, task))
            continue;
        task.mergeOffered = true;
        // The setting at the moment the task can be merged applies (ADR 0114 §5).
        if (team.mergePolicy == QStringLiteral("user")) {
            raiseDecision(team, QStringLiteral("merge"),
                          QStringLiteral("Merge %1 \"%2\"?").arg(task.id, task.title),
                          QStringLiteral("Reviewed with no findings. Branch %1 into %2.")
                              .arg(task.branch, team.integrationBranch),
                          {QStringLiteral("I merge it myself"), leaderMergesChoice},
                          leaderMergesChoice, task.id, QStringLiteral("daemon"));
            addHistory(task, QStringLiteral("daemon"), QStringLiteral("merge"),
                       QStringLiteral("Waits for the user to merge, or to let the leader."));
            updates.append(
                {.kind = QStringLiteral("merge_waiting"),
                 .text = QStringLiteral("%1 waits for the user's merge decision.").arg(task.id),
                 .taskId = task.id});
        } else {
            addHistory(task, QStringLiteral("daemon"), QStringLiteral("merge"),
                       QStringLiteral("Ready for the leader to merge into %1.")
                           .arg(team.integrationBranch));
            updates.append({.kind = QStringLiteral("merge_ready"),
                            .text = QStringLiteral("%1 can be merged into %2: %3.")
                                        .arg(task.id, team.integrationBranch, task.branch),
                            .taskId = task.id,
                            .actionable = true});
        }
    }
}

void TeamCoordinator::checkMerged(const qint64 teamId) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    QStringList merged;
    // A ready task was reviewed at one commit (ADR 0114 §5). Commits added to its branch
    // after that were never reviewed.
    QHash<QString, QString> moved;
    for (const Task& task : team->tasks) {
        if (task.state != state::ready || task.branch.isEmpty())
            continue;
        if (const QString head = host_->headCommit(task.worktree);
            !head.isEmpty() && !task.headCommit.isEmpty() && head != task.headCommit)
            moved.insert(task.id, head);
        if (const auto contained = host_->isMerged(*team, task); contained && *contained)
            merged.append(task.id);
    }
    if (merged.isEmpty() && moved.isEmpty())
        return;
    team = mutableTeam(teamId);
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    QList<Update> updates;
    const auto withdrawMerge = [&](const QString& id, const QString& reason) {
        // A merge decision nobody needs any more, as when the user merged by hand.
        for (Decision& decision : team->decisions)
            if (decision.kind == QStringLiteral("merge") && decision.taskId == id &&
                decision.state == QStringLiteral("open")) {
                decision.state = QStringLiteral("withdrawn");
                decision.reason = reason;
                decision.answeredAt = nowIso();
            }
    };
    for (const QString& id : merged) {
        Task* task = team->task(id);
        const QString head = moved.take(id);
        applyMove(*team, *task, state::done, QStringLiteral("daemon"),
                  QStringLiteral("%1 contains it").arg(team->integrationBranch), updates);
        withdrawMerge(id, QStringLiteral("%1 is merged.").arg(id));
        if (head.isEmpty())
            continue;
        // Too late to stop it; the leader is told what went in without a review.
        const QString text = QStringLiteral("%1 was merged at %2, but it was reviewed at %3: the "
                                            "commits after that were not reviewed.")
                                 .arg(id, head.left(12), task->headCommit.left(12));
        addHistory(*task, QStringLiteral("daemon"), QStringLiteral("merge"), text);
        updates.append({.kind = QStringLiteral("merged_unreviewed"),
                        .text = text,
                        .taskId = id,
                        .actionable = true});
    }
    for (auto it = moved.cbegin(); it != moved.cend(); ++it) {
        Task* task = team->task(it.key());
        task->mergeOffered = false;
        task->leaderMayMerge = false;
        task->resumeState = state::inProgress;
        task->question = QStringLiteral("%1 was reviewed at %2 and its branch is now at %3. "
                                        "Start it again and hand it to review, so the new "
                                        "commits are reviewed before it is merged.")
                             .arg(task->id, task->headCommit.left(12), it.value().left(12));
        applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question, updates);
        withdrawMerge(task->id, QStringLiteral("%1 changed after its review.").arg(task->id));
    }
    offerMerges(*team, updates);
    const bool allDone = !team->tasks.isEmpty() &&
                         std::all_of(team->tasks.cbegin(), team->tasks.cend(),
                                     [](const Task& task) { return task.state == state::done; });
    if (allDone)
        updates.append({.kind = QStringLiteral("goal_reached"),
                        .text = QStringLiteral("Every task is done. Report to the user once what "
                                               "%1 contains; merging it onward is the user's.")
                                    .arg(team->integrationBranch),
                        .actionable = true});
    if (commit(*team, before, QStringLiteral("daemon"), updates))
        qWarning("Unable to record the merged tasks of team %lld.", static_cast<long long>(teamId));
}

} // namespace ketplus::daemon::teams
