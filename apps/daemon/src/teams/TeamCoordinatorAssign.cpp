// ADR 0114 §6 and open point 1: who owns a task. An assignment and a handover give the task
// its worktree the first time and point the owner's conversation at it.
#include "teams/TeamCoordinator.h"

#include <QDir>
#include <QPointer>

namespace ketplus::daemon::teams {
namespace {

TeamError refusal(const QString& code, const QString& message) { return {code, message}; }

TeamError notFound() {
    return refusal(QStringLiteral("team_not_found"), QStringLiteral("The team no longer exists."));
}

TeamError taskNotFound(const QString& id) {
    return refusal(QStringLiteral("task_not_found"),
                   QStringLiteral("There is no task %1.").arg(id));
}

TeamError forbidden(const QString& action, const QString& who) {
    return refusal(QStringLiteral("role_forbidden"),
                   QStringLiteral("Only %1 may %2.").arg(who, action));
}

bool leaderOrUser(const QString& role) {
    return role == role::leader || role == QStringLiteral("user");
}

} // namespace

std::optional<TeamError> TeamCoordinator::assignRefusal(const Team& team, const Task& task,
                                                        const QString& owner,
                                                        const QString& actor) const {
    if (!leaderOrUser(actorRole(team, actor)))
        return forbidden(QStringLiteral("assign tasks"), QStringLiteral("the leader"));
    const Member* member = team.member(owner);
    if (!member || member->role != role::developer)
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("A task is owned by a developer of the team."));
    if (!task.owner.isEmpty())
        return refusal(QStringLiteral("invalid_transition"),
                       QStringLiteral("%1 has an owner; hand it over instead.").arg(task.id));
    if (task.state != state::todo)
        return refusal(QStringLiteral("invalid_transition"),
                       QStringLiteral("Only a task in To do is assigned."));
    return std::nullopt;
}

TeamResult TeamCoordinator::assignTask(const qint64 teamId, const QString& taskId,
                                       const QString& owner, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task)
        return {.error = taskNotFound(taskId)};
    if (const auto refused = assignRefusal(*team, *task, owner, actor))
        return {.error = refused};
    const Member* member = team->member(owner);
    if (task->branch.isEmpty()) {
        const WorktreeAnswer made = worktreeFor(*team, *task);
        if (!made.worktree)
            return {.error = refusal(QStringLiteral("worktree_unavailable"), made.error)};
        team = mutableTeam(teamId);
        task = team->task(taskId);
        member = team->member(owner);
        takeWorktree(*team, *task, *made.worktree);
    }
    task->owner = owner;
    addHistory(*task, actor, QStringLiteral("owner"),
               say(team->language, QT_TRANSLATE_NOOP("Team", "Assigned to %1 by %2."))
                   .arg(member->name, actorName(*team, actor)),
               {{QStringLiteral("from"), QString{}}, {QStringLiteral("to"), owner}});
    const QString name = member->name;
    if (auto failed =
            commit(*team, before, actor,
                   {{.kind = QStringLiteral("task_assigned"),
                     .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 assigned to %2."))
                                 .arg(taskId, name),
                     .taskId = taskId,
                     .data = {{QStringLiteral("owner"), owner}}}}))
        return {.error = failed};
    if (!holdForFirstPlan(teamId, taskId, actor))
        deliverAssignment(*team, *team->task(taskId), actor, false);
    return {.teamId = teamId};
}

std::optional<TeamError> TeamCoordinator::handoverRefusal(const Team& team, const Task& task,
                                                          const QString& owner,
                                                          const QString& reason,
                                                          const QString& actor) const {
    if (!leaderOrUser(actorRole(team, actor)))
        return forbidden(QStringLiteral("hand tasks over"), QStringLiteral("the leader"));
    if (reason.trimmed().isEmpty())
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("A handover needs a reason."));
    const Member* member = team.member(owner);
    if (!member || member->role != role::developer)
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("A task is owned by a developer of the team."));
    if (task.owner == owner)
        return refusal(QStringLiteral("invalid_transition"),
                       QStringLiteral("%1 already owns %2.").arg(member->name, task.id));
    if (task.state == state::done)
        return refusal(QStringLiteral("invalid_transition"),
                       QStringLiteral("%1 is done.").arg(task.id));
    if (task.state == state::inProgress)
        for (const Task& other : team.tasks)
            if (other.owner == owner && other.state == state::inProgress)
                return refusal(QStringLiteral("invalid_transition"),
                               QStringLiteral("%1 is working on %2.").arg(member->name, other.id));
    return std::nullopt;
}

TeamResult TeamCoordinator::handoverTask(const qint64 teamId, const QString& taskId,
                                         const QString& owner, const QString& reason,
                                         const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task)
        return {.error = taskNotFound(taskId)};
    if (const auto refused = handoverRefusal(*team, *task, owner, reason, actor))
        return {.error = refused};
    const Member* member = team->member(owner);
    const QString previous = task->owner;
    if (task->branch.isEmpty()) {
        const WorktreeAnswer made = worktreeFor(*team, *task);
        if (!made.worktree)
            return {.error = refusal(QStringLiteral("worktree_unavailable"), made.error)};
        team = mutableTeam(teamId);
        task = team->task(taskId);
        member = team->member(owner);
        takeWorktree(*team, *task, *made.worktree);
    }
    // The previous owner must not stay in a worktree that is someone else's now. Only a
    // conversation that stands in it has to leave, and the move is stored with the handover:
    // a conversation that cannot move during its turn leaves when the turn ends.
    if (!previous.isEmpty() && team->member(previous) && !task->worktree.isEmpty()) {
        const auto conversation = host_->teamConversation(previous);
        const Relocation move{.session = previous, .from = task->worktree};
        if (conversation && QDir::cleanPath(conversation->worktree) == QDir::cleanPath(move.from) &&
            !team->relocations.contains(move))
            team->relocations.append(move);
    }
    // One assignment: the owner changes in the same commit, so a failure leaves the old one.
    task->owner = owner;
    addHistory(*task, actor, QStringLiteral("owner"),
               say(team->language, QT_TRANSLATE_NOOP("Team", "Handed over from %1 to %2: %3"))
                   .arg(previous.isEmpty()
                            ? say(team->language, QT_TRANSLATE_NOOP("Team", "nobody"))
                            : actorName(*team, previous),
                        member->name, reason.trimmed()),
               {{QStringLiteral("from"), previous},
                {QStringLiteral("to"), owner},
                {QStringLiteral("reason"), reason.trimmed()}});
    if (auto failed = commit(
            *team, before, actor,
            {{.kind = QStringLiteral("task_handover"),
              .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 handed over to %2: %3"))
                          .arg(taskId, member->name, reason.trimmed()),
              .taskId = taskId,
              .data = {{QStringLiteral("from"), previous}, {QStringLiteral("to"), owner}}}}))
        return {.error = failed};
    if (!previous.isEmpty())
        host_->noteInConversation(previous,
                                  {{QStringLiteral("kind"), QStringLiteral("team")},
                                   {QStringLiteral("state"), QStringLiteral("task_handed_over")},
                                   {QStringLiteral("task_id"), taskId},
                                   {QStringLiteral("body"),
                                    QStringLiteral("%1 is no longer yours: it was handed to %2. %3")
                                        .arg(taskId, member->name, reason.trimmed())}});
    // T-32: the previous owner leaves the worktree after the reply; its git runs off this
    // thread. A switch that cannot happen yet is noted on the task and tried again when that
    // member's turn ends.
    if (!previous.isEmpty()) {
        settleRelocations(
            teamId, previous,
            [self = QPointer<TeamCoordinator>(this), teamId, taskId](const QString& waiting) {
                Team* stored = self && !waiting.isEmpty() ? self->mutableTeam(teamId) : nullptr;
                Task* task = stored ? stored->task(taskId) : nullptr;
                if (!task)
                    return;
                const Team unmoved = *stored;
                addHistory(*task, QStringLiteral("daemon"), QStringLiteral("worktree"),
                           say(stored->language,
                               QT_TRANSLATE_NOOP("Team", "The previous owner's conversation leaves "
                                                         "%1 when its turn ends: %2"))
                               .arg(task->worktree, waiting));
                (void)self->commit(*stored, unmoved, QStringLiteral("daemon"), {});
            });
        team = mutableTeam(teamId);
    }
    // A task held for the first plan stays held with its new owner.
    if (!team->task(taskId)->assignmentHeld)
        deliverAssignment(*team, *team->task(taskId), actor, true, reason.trimmed());
    return {.teamId = teamId};
}

} // namespace ketplus::daemon::teams
