// ADR 0114 §1: the git a task command asks for, read before the command runs.
#include "teams/TeamCoordinator.h"

#include <QPointer>

#include <utility>

namespace ketplus::daemon::teams {
namespace {

QString taskKey(const qint64 teamId, const QString& taskId) {
    return QString::number(teamId) + QLatin1Char('/') + taskId;
}

} // namespace

void TeamCoordinator::prepare(const TaskRequest& request, std::function<void()> then) {
    const Team* team = this->team(request.teamId);
    const Task* task = team ? team->task(request.taskId) : nullptr;
    if (!task) {
        then();
        return;
    }
    const QString key = taskKey(request.teamId, request.taskId);
    if (request.kind == TaskRequest::Kind::Move) {
        // Only a hand to review reads git, and only its owner's or the daemon's gets that far.
        const bool mayHand = (!task->owner.isEmpty() && task->owner == request.actor) ||
                             actorRole(*team, request.actor) == QStringLiteral("daemon");
        if (request.to != state::inReview || task->state != state::inProgress || !mayHand) {
            then();
            return;
        }
        ++handoffsAsked_;
        host_->askHandoff(*team, *task,
                          [self = QPointer<TeamCoordinator>(this), key,
                           then = std::move(then)](const HandoffFacts& facts) {
                              if (!self)
                                  return;
                              --self->handoffsAsked_;
                              // There for the command `then` runs, and for no other.
                              self->handoffs_.insert(key, facts);
                              then();
                              if (self)
                                  self->handoffs_.remove(key);
                          });
        return;
    }
    const auto refused =
        request.kind == TaskRequest::Kind::Assign
            ? assignRefusal(*team, *task, request.owner, request.actor)
            : handoverRefusal(*team, *task, request.owner, request.reason, request.actor);
    if (refused || !task->branch.isEmpty()) {
        then();
        return;
    }
    QList<std::function<void()>>& waiting = worktreeWaits_[key];
    waiting.append(std::move(then));
    if (waiting.size() > 1)
        return;
    host_->askWorktree(*team, *task,
                       [self = QPointer<TeamCoordinator>(this), teamId = request.teamId,
                        taskId = request.taskId](const WorktreeAnswer& answer) {
                           if (self)
                               self->worktreeAnswered(teamId, taskId, answer);
                       });
}

void TeamCoordinator::worktreeAnswered(const qint64 teamId, const QString& taskId,
                                       const WorktreeAnswer& answer) {
    const QString key = taskKey(teamId, taskId);
    const QList<std::function<void()>> waiting = worktreeWaits_.take(key);
    // Every command that waited finds the one answer: the first that still may takes the
    // worktree, and a failure is each one's refusal instead of a question of its own.
    worktrees_.insert(key, answer);
    const QPointer<TeamCoordinator> self(this);
    for (const auto& then : waiting) {
        then();
        if (!self)
            return;
    }
    worktrees_.remove(key);
    if (!answer.worktree)
        return;
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task || !task->branch.isEmpty())
        return;
    // Git made the worktree and no command took it: each was refused by what the team became
    // meanwhile. The task keeps it, because the path and the branch are taken now and its next
    // assignment could not make them again.
    task->branch = answer.worktree->branch;
    task->worktree = answer.worktree->worktree;
    task->baseCommit = answer.worktree->baseCommit;
    addHistory(*task, QStringLiteral("daemon"), QStringLiteral("worktree"),
               QStringLiteral("The worktree %1 was made for an assignment that was refused "
                              "meanwhile; the task keeps it.")
                   .arg(task->worktree));
    (void)commit(*team, before, QStringLiteral("daemon"), {});
}

WorktreeAnswer TeamCoordinator::worktreeFor(const Team& team, const Task& task) {
    if (const auto prepared = worktrees_.constFind(taskKey(team.id, task.id));
        prepared != worktrees_.cend())
        return prepared.value();
    WorktreeAnswer answer;
    answer.worktree = host_->createTaskWorktree(team, task, &answer.error);
    return answer;
}

HandoffFacts TeamCoordinator::handoffFor(const Team& team, const Task& task) const {
    // An answer read for another worktree or branch than the task has now says nothing of it.
    if (const auto prepared = handoffs_.constFind(taskKey(team.id, task.id));
        prepared != handoffs_.cend() && prepared->worktree == task.worktree &&
        prepared->branch == task.branch)
        return prepared.value();
    return {.worktree = task.worktree,
            .branch = task.branch,
            .dirty = host_->hasUncommittedWork(task.worktree),
            .head = host_->taskCommit(team, task)};
}

} // namespace ketplus::daemon::teams
