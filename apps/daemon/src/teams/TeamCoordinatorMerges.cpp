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
            const QString leaderMerges = say(team.language, leaderMergesSource);
            raiseDecision(
                team, QStringLiteral("merge"),
                say(team.language, QT_TRANSLATE_NOOP("Team", "Merge %1 \"%2\"?"))
                    .arg(task.id, task.title),
                say(team.language,
                    QT_TRANSLATE_NOOP("Team", "Reviewed with no findings. Branch %1 into %2."))
                    .arg(task.branch, taskScope(team, task).integrationBranch),
                {say(team.language, QT_TRANSLATE_NOOP("Team", "I merge it myself")), leaderMerges},
                leaderMerges, task.id, QStringLiteral("daemon"));
            addHistory(task, QStringLiteral("daemon"), QStringLiteral("merge"),
                       say(team.language,
                           QT_TRANSLATE_NOOP(
                               "Team", "Waits for the user to merge, or to let the leader.")));
            updates.append(
                {.kind = QStringLiteral("merge_waiting"),
                 .text = say(team.language,
                             QT_TRANSLATE_NOOP("Team", "%1 waits for the user's merge decision."))
                             .arg(task.id),
                 .taskId = task.id});
        } else {
            addHistory(task, QStringLiteral("daemon"), QStringLiteral("merge"),
                       say(team.language,
                           QT_TRANSLATE_NOOP("Team", "Ready for the leader to merge into %1."))
                           .arg(taskScope(team, task).integrationBranch));
            updates.append({.kind = QStringLiteral("merge_ready"),
                            .text = say(team.language,
                                        QT_TRANSLATE_NOOP("Team", "%1 can be merged into %2: %3."))
                                        .arg(task.id, taskScope(team, task).integrationBranch, task.branch),
                            .taskId = task.id,
                            .actionable = true});
        }
    }
}

void TeamCoordinator::checkMerged(const qint64 teamId) {
    // A merge through a pull request is the remote's to tell (ADR 0118).
    checkPullRequests(teamId);
    const Team* team = this->team(teamId);
    if (!team)
        return;
    QList<Task> ready;
    for (const Task& task : team->tasks)
        if (task.state == state::ready && !task.branch.isEmpty())
            ready.append(task);
    if (ready.isEmpty())
        return;
    if (mergeChecks_.contains(teamId)) {
        mergeChecksAgain_.insert(teamId);
        return;
    }
    mergeChecks_.insert(teamId);
    host_->askTasks(
        *team, ready, TaskQuestion::Merge,
        [self = QPointer<TeamCoordinator>(this), teamId, ready, repository = team->repository,
         integration = team->integrationBranch](const QHash<QString, TaskFacts>& facts) {
            if (!self)
                return;
            self->mergeChecks_.remove(teamId);
            bool again = self->mergeChecksAgain_.remove(teamId);
            // Git answered for the integration branch the team had. An answer
            // about another one says nothing: it is dropped and asked again.
            const Team* now = self->team(teamId);
            if (now && now->repository == repository && now->integrationBranch == integration)
                self->applyMergeFacts(teamId, ready, facts);
            else
                again = now != nullptr;
            if (again)
                self->checkMerged(teamId);
        });
}

void TeamCoordinator::applyMergeFacts(const qint64 teamId, const QList<Task>& asked,
                                      const QHash<QString, TaskFacts>& facts) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    QStringList merged;
    // A ready task was reviewed at one commit (ADR 0114 §5). Commits added to its branch
    // after that were never reviewed.
    QHash<QString, QString> moved;
    for (const Task& was : asked) {
        const Task* task = std::as_const(*team).task(was.id);
        const auto fact = facts.constFind(was.id);
        // Git was asked about the task as it was. One that moved on meanwhile is asked
        // about again by the next check.
        if (!task || fact == facts.cend() || task->state != state::ready ||
            task->branch != was.branch || task->headCommit != was.headCommit)
            continue;
        // T-38: git speaks only of a branch with commits of its own, handed to review at one
        // of them. A branch still at its base is contained in any branch it was cut from: its
        // work is elsewhere, as in another repository's pull request (ADR 0118).
        if (!branchHasOwnCommits(*task, fact->head) || !reviewedOwnCommit(*task))
            continue;
        if (fact->head != task->headCommit)
            moved.insert(task->id, fact->head);
        // A task that records a pull request is the pull request's to finish (ADR 0118): its
        // branch can be contained without its work being merged, as after it took in the
        // integration branch, or when the work is in another repository.
        if (fact->merged && *fact->merged && !recordsPullRequest(*task))
            merged.append(task->id);
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
        // T-90: the integration branch of the repository the task works in.
        const QString integration = taskScope(*team, *task).integrationBranch;
        finishTask(*team, *task, QStringLiteral("daemon"),
                   QStringLiteral("%1 contains it").arg(integration), QStringLiteral("merged"),
                   {{QStringLiteral("branch"), task->branch},
                    {QStringLiteral("integration_branch"), integration},
                    {QStringLiteral("commit"), head.isEmpty() ? task->headCommit : head}},
                   updates);
        if (head.isEmpty())
            continue;
        // Too late to stop it; the leader is told what went in without a review.
        const QString text =
            say(team->language, QT_TRANSLATE_NOOP("Team", "%1 was merged at %2, but it was "
                                                          "reviewed at %3: the commits after "
                                                          "that were not reviewed."))
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
        task->question =
            say(team->language,
                QT_TRANSLATE_NOOP("Team", "%1 was reviewed at %2 and its branch is now at %3. "
                                          "Start it again and hand it to review, so the new "
                                          "commits are reviewed before it is merged."))
                .arg(task->id, task->headCommit.left(12), it.value().left(12));
        applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question, updates);
        withdrawMerge(task->id,
                      say(team->language, QT_TRANSLATE_NOOP("Team", "%1 changed after its review."))
                          .arg(task->id));
    }
    offerMerges(*team, updates);
    if (!merged.isEmpty())
        noteGoalReached(*team, updates);
    if (commit(*team, before, QStringLiteral("daemon"), updates))
        qWarning("Unable to record the merged tasks of team %lld.", static_cast<long long>(teamId));
}

} // namespace ketplus::daemon::teams
