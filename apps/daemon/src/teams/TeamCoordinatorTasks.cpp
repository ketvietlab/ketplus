// ADR 0114 §6: tasks with one owner, states, criteria and an append-only history.
#include "teams/TeamCoordinator.h"

#include "storage/LocalJournal.h"

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

std::optional<TeamError> validText(const QString& text, const QString& what, int maximumBytes) {
    if (text.trimmed().isEmpty())
        return refusal(QStringLiteral("invalid_payload"), QStringLiteral("%1 is empty.").arg(what));
    if (text.toUtf8().size() > maximumBytes)
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("%1 is longer than %2 bytes.").arg(what).arg(maximumBytes));
    return std::nullopt;
}

} // namespace

QString TeamCoordinator::actorRole(const Team& team, const QString& actor) const {
    if (actor == QStringLiteral("user") || actor == QStringLiteral("daemon"))
        return actor;
    const Member* member = team.member(actor);
    return member ? member->role : QString{};
}

QString TeamCoordinator::actorName(const Team& team, const QString& actor) const {
    if (actor == QStringLiteral("user"))
        return QStringLiteral("the user");
    if (actor == QStringLiteral("daemon"))
        return QStringLiteral("KetPlus");
    const Member* member = team.member(actor);
    return member ? member->name : actor;
}

TeamResult TeamCoordinator::createTask(const qint64 teamId, const TaskDraft& draft,
                                       const QString& actor, QString* taskId) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    const QString role = actorRole(*team, actor);
    if (!leaderOrUser(role))
        return {.error = forbidden(QStringLiteral("create tasks"), QStringLiteral("the leader"))};
    if (auto invalid = validText(draft.title, QStringLiteral("The title"), 300))
        return {.error = invalid};
    if (draft.description.toUtf8().size() > 16 * 1024)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("The description is longer than 16 KiB."))};
    if (draft.criteria.size() > maximumCriteria)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("A task has at most 20 criteria."))};
    if (!draft.criteria.isEmpty() && role == role::leader && !team->leaderWritesRequirements)
        return {.error = forbidden(QStringLiteral("write the criteria of this team"),
                                   QStringLiteral("the user"))};
    for (const QString& criterion : draft.criteria)
        if (auto invalid = validText(criterion, QStringLiteral("A criterion"), 2000))
            return {.error = invalid};
    for (const QString& dependency : draft.dependsOn)
        if (!team->task(dependency))
            return {.error = taskNotFound(dependency)};
    Task task;
    task.id = QStringLiteral("T-%1").arg(team->nextTask++);
    task.title = draft.title.trimmed();
    task.description = draft.description.trimmed();
    task.lane = draft.lane.trimmed();
    task.dependsOn = draft.dependsOn;
    for (const QString& criterion : draft.criteria)
        task.criteria.append({.text = criterion.trimmed()});
    addHistory(task, actor, QStringLiteral("created"),
               QStringLiteral("Created by %1.").arg(actorName(*team, actor)));
    team->tasks.append(task);
    if (auto failed =
            commit(*team, before, actor,
                   {{.kind = QStringLiteral("task_created"),
                     .text = QStringLiteral("%1 \"%2\" created.").arg(task.id, task.title),
                     .taskId = task.id}}))
        return {.error = failed};
    if (taskId)
        *taskId = task.id;
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::updateTask(const qint64 teamId, const QString& taskId,
                                       const QJsonObject& changes, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task)
        return {.error = taskNotFound(taskId)};
    const QString role = actorRole(*team, actor);
    const bool owner = !task->owner.isEmpty() && task->owner == actor;
    QList<Update> updates;
    const auto restore = [&](const TeamError& error) {
        *team = before;
        return TeamResult{.error = error};
    };
    for (const QString& key : {QStringLiteral("title"), QStringLiteral("description"),
                               QStringLiteral("lane"), QStringLiteral("depends_on")}) {
        if (!changes.contains(key))
            continue;
        if (!leaderOrUser(role))
            return restore(forbidden(QStringLiteral("change a task's %1").arg(key),
                                     QStringLiteral("the leader")));
    }
    if (changes.contains(QStringLiteral("title"))) {
        const QString title = changes.value(QStringLiteral("title")).toString().trimmed();
        if (auto invalid = validText(title, QStringLiteral("The title"), 300))
            return restore(*invalid);
        task->title = title;
    }
    if (changes.contains(QStringLiteral("description")))
        task->description =
            changes.value(QStringLiteral("description")).toString().trimmed().left(16000);
    if (changes.contains(QStringLiteral("lane"))) {
        const QString lane = changes.value(QStringLiteral("lane")).toString().trimmed();
        if (lane != task->lane)
            addHistory(*task, actor, QStringLiteral("lane"),
                       QStringLiteral("Lane %1 → %2.").arg(task->lane, lane));
        task->lane = lane;
    }
    if (changes.contains(QStringLiteral("depends_on"))) {
        QStringList dependencies;
        for (const auto& value : changes.value(QStringLiteral("depends_on")).toArray()) {
            const QString dependency = value.toString();
            if (dependency == task->id || !team->task(dependency))
                return restore(taskNotFound(dependency));
            dependencies.append(dependency);
        }
        // A task waits for its dependencies to be done; in a circle none of them starts.
        QStringList open = dependencies;
        QSet<QString> seen;
        while (!open.isEmpty()) {
            const QString next = open.takeLast();
            if (next == taskId)
                return restore(refusal(
                    QStringLiteral("dependency_cycle"),
                    QStringLiteral("%1 cannot wait for a task that waits for %1.").arg(taskId)));
            if (seen.contains(next))
                continue;
            seen.insert(next);
            if (const Task* other = team->task(next))
                open.append(other->dependsOn);
        }
        task = team->task(taskId);
        task->dependsOn = dependencies;
    }
    if (changes.contains(QStringLiteral("pull_request")) ||
        changes.contains(QStringLiteral("note"))) {
        if (!owner && !leaderOrUser(role))
            return restore(
                refusal(QStringLiteral("not_owner"),
                        QStringLiteral("Only the owner of %1 reports on it.").arg(taskId)));
        if (changes.contains(QStringLiteral("pull_request")))
            task->pullRequest =
                changes.value(QStringLiteral("pull_request")).toString().trimmed().left(500);
        const QString note = changes.value(QStringLiteral("note")).toString().trimmed();
        if (!note.isEmpty()) {
            addHistory(*task, actor, QStringLiteral("note"), note.left(4000));
            updates.append({.kind = QStringLiteral("note"),
                            .text = QStringLiteral("%1: %2").arg(task->id, note.left(500)),
                            .taskId = task->id});
        }
    }
    if (changes.contains(QStringLiteral("met"))) {
        if (!owner && role != QStringLiteral("user"))
            return restore(
                refusal(QStringLiteral("not_owner"),
                        QStringLiteral("Only the owner of %1 marks its criteria.").arg(taskId)));
        for (const auto& value : changes.value(QStringLiteral("met")).toArray()) {
            const int index = value.toObject().value(QStringLiteral("index")).toInt(-1);
            if (index < 0 || index >= task->criteria.size())
                return restore(
                    refusal(QStringLiteral("invalid_payload"),
                            QStringLiteral("%1 has no criterion %2.").arg(taskId).arg(index)));
            const bool met = value.toObject().value(QStringLiteral("met")).toBool();
            if (task->criteria[index].met == met)
                continue;
            task->criteria[index].met = met;
            addHistory(*task, actor, QStringLiteral("met"),
                       QStringLiteral("%1 criterion %2: %3")
                           .arg(met ? QStringLiteral("Met") : QStringLiteral("Not met"))
                           .arg(index + 1)
                           .arg(task->criteria[index].text.left(200)),
                       {{QStringLiteral("index"), index}, {QStringLiteral("met"), met}});
            updates.append(
                {.kind = QStringLiteral("met"),
                 .text = QStringLiteral("%1 criterion %2 %3.")
                             .arg(task->id)
                             .arg(index + 1)
                             .arg(met ? QStringLiteral("met") : QStringLiteral("not met")),
                 .taskId = task->id});
        }
    }
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::setCriteria(const qint64 teamId, const QString& taskId,
                                        const QStringList& texts, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task)
        return {.error = taskNotFound(taskId)};
    const QString role = actorRole(*team, actor);
    if (!leaderOrUser(role))
        return {.error = forbidden(QStringLiteral("change the text of the criteria"),
                                   QStringLiteral("the leader"))};
    if (role == role::leader && !team->leaderWritesRequirements)
        return {.error = forbidden(QStringLiteral("change the criteria of this team, whose "
                                                  "requirements are written elsewhere"),
                                   QStringLiteral("the user"))};
    if (texts.size() > maximumCriteria)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("A task has at most 20 criteria."))};
    for (const QString& text : texts)
        if (auto invalid = validText(text, QStringLiteral("A criterion"), 2000))
            return {.error = invalid};
    QStringList old;
    QList<Criterion> criteria;
    for (const Criterion& criterion : task->criteria)
        old.append(criterion.text);
    for (const QString& text : texts) {
        const QString trimmed = text.trimmed();
        bool met = false;
        for (const Criterion& criterion : task->criteria)
            if (criterion.text == trimmed)
                met = criterion.met; // An unchanged criterion keeps its mark.
        criteria.append({.text = trimmed, .met = met});
    }
    task->criteria = criteria;
    QStringList now;
    for (const Criterion& criterion : criteria)
        now.append(criterion.text);
    addHistory(*task, actor, QStringLiteral("criteria"),
               QStringLiteral("Criteria changed by %1.").arg(actorName(*team, actor)),
               {{QStringLiteral("from"), QJsonArray::fromStringList(old)},
                {QStringLiteral("to"), QJsonArray::fromStringList(now)}});
    if (auto failed = commit(*team, before, actor,
                             {{.kind = QStringLiteral("criteria"),
                               .text = QStringLiteral("%1 criteria changed.").arg(task->id),
                               .taskId = task->id}}))
        return {.error = failed};
    return {.teamId = teamId};
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
    if (!leaderOrUser(actorRole(*team, actor)))
        return {.error = forbidden(QStringLiteral("assign tasks"), QStringLiteral("the leader"))};
    const Member* member = team->member(owner);
    if (!member || member->role != role::developer)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("A task is owned by a developer of the team."))};
    if (!task->owner.isEmpty())
        return {.error =
                    refusal(QStringLiteral("invalid_transition"),
                            QStringLiteral("%1 has an owner; hand it over instead.").arg(taskId))};
    if (task->state != state::todo)
        return {.error = refusal(QStringLiteral("invalid_transition"),
                                 QStringLiteral("Only a task in To do is assigned."))};
    if (task->branch.isEmpty()) {
        QString error;
        const auto worktree = host_->createTaskWorktree(*team, *task, &error);
        if (!worktree)
            return {.error = refusal(QStringLiteral("worktree_unavailable"), error)};
        team = mutableTeam(teamId);
        task = team->task(taskId);
        task->branch = worktree->branch;
        task->worktree = worktree->worktree;
        task->baseCommit = worktree->baseCommit;
    }
    task->owner = owner;
    addHistory(*task, actor, QStringLiteral("owner"),
               QStringLiteral("Assigned to %1 by %2.").arg(member->name, actorName(*team, actor)),
               {{QStringLiteral("from"), QString{}}, {QStringLiteral("to"), owner}});
    const QString name = member->name;
    if (auto failed = commit(*team, before, actor,
                             {{.kind = QStringLiteral("task_assigned"),
                               .text = QStringLiteral("%1 assigned to %2.").arg(taskId, name),
                               .taskId = taskId,
                               .data = {{QStringLiteral("owner"), owner}}}}))
        return {.error = failed};
    if (!holdForFirstPlan(teamId, taskId, actor))
        deliverAssignment(*team, *team->task(taskId), actor, false);
    return {.teamId = teamId};
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
    if (!leaderOrUser(actorRole(*team, actor)))
        return {.error =
                    forbidden(QStringLiteral("hand tasks over"), QStringLiteral("the leader"))};
    if (reason.trimmed().isEmpty())
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("A handover needs a reason."))};
    const Member* member = team->member(owner);
    if (!member || member->role != role::developer)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("A task is owned by a developer of the team."))};
    if (task->owner == owner)
        return {.error = refusal(QStringLiteral("invalid_transition"),
                                 QStringLiteral("%1 already owns %2.").arg(member->name, taskId))};
    if (task->state == state::done)
        return {.error = refusal(QStringLiteral("invalid_transition"),
                                 QStringLiteral("%1 is done.").arg(taskId))};
    if (task->state == state::inProgress)
        for (const Task& other : team->tasks)
            if (other.owner == owner && other.state == state::inProgress)
                return {.error = refusal(
                            QStringLiteral("invalid_transition"),
                            QStringLiteral("%1 is working on %2.").arg(member->name, other.id))};
    const QString previous = task->owner;
    if (task->branch.isEmpty()) {
        QString error;
        const auto worktree = host_->createTaskWorktree(*team, *task, &error);
        if (!worktree)
            return {.error = refusal(QStringLiteral("worktree_unavailable"), error)};
        team = mutableTeam(teamId);
        task = team->task(taskId);
        task->branch = worktree->branch;
        task->worktree = worktree->worktree;
        task->baseCommit = worktree->baseCommit;
    }
    // One assignment: the owner changes in the same commit, so a failure leaves the old one.
    task->owner = owner;
    addHistory(*task, actor, QStringLiteral("owner"),
               QStringLiteral("Handed over from %1 to %2: %3")
                   .arg(previous.isEmpty() ? QStringLiteral("nobody") : actorName(*team, previous),
                        member->name, reason.trimmed()),
               {{QStringLiteral("from"), previous},
                {QStringLiteral("to"), owner},
                {QStringLiteral("reason"), reason.trimmed()}});
    if (auto failed =
            commit(*team, before, actor,
                   {{.kind = QStringLiteral("task_handover"),
                     .text = QStringLiteral("%1 handed over to %2: %3")
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
    // The previous owner must not stay in a worktree that is someone else's now: it goes to
    // the task it still works on, or back to the repository.
    if (!previous.isEmpty() && team->member(previous)) {
        const Task handed = *team->task(taskId);
        QString home = team->repository;
        for (const Task& other : team->tasks)
            if (other.owner == previous && other.state == state::inProgress &&
                !other.worktree.isEmpty())
                home = other.worktree;
        if (!handed.worktree.isEmpty() && home != handed.worktree) {
            if (const QString error = host_->pointConversationAt(previous, home);
                !error.isEmpty()) {
                Team* stored = mutableTeam(teamId);
                const Team unmoved = *stored;
                addHistory(*stored->task(taskId), QStringLiteral("daemon"),
                           QStringLiteral("worktree"),
                           QStringLiteral("The previous owner's conversation was not moved out "
                                          "of %1: %2")
                               .arg(handed.worktree, error));
                (void)commit(*stored, unmoved, QStringLiteral("daemon"), {});
            }
            team = mutableTeam(teamId);
        }
    }
    // A task held for the first plan stays held with its new owner.
    if (!team->task(taskId)->assignmentHeld)
        deliverAssignment(*team, *team->task(taskId), actor, true, reason.trimmed());
    return {.teamId = teamId};
}

void TeamCoordinator::applyMove(Team& team, Task& task, const QString& to, const QString& actor,
                                const QString& reason, QList<Update>& updates) {
    const QString from = task.state;
    task.state = to;
    addHistory(task, actor, QStringLiteral("state"),
               QStringLiteral("%1 → %2%3")
                   .arg(from, to, reason.isEmpty() ? QString{} : QStringLiteral(": ") + reason),
               {{QStringLiteral("from"), from}, {QStringLiteral("to"), to}});
    const bool actionable = to == state::ready || to == state::blocked || to == state::done;
    updates.append({.kind = QStringLiteral("state"),
                    .text = QStringLiteral("%1 %2 → %3%4")
                                .arg(task.id, from, to,
                                     reason.isEmpty() ? QString{} : QStringLiteral(": ") + reason),
                    .taskId = task.id,
                    .actionable = actionable,
                    .data = {{QStringLiteral("from"), from}, {QStringLiteral("to"), to}}});
    // ADR 0114 §2: the reviewer takes tasks first in, first out; commit() sends the next.
    if (to == state::inReview && !team.reviewQueue.contains(task.id) && team.reviewing != task.id)
        team.reviewQueue.append(task.id);
    // A developer whose work all left its hands needs the leader (ADR 0114 §7).
    if (!task.owner.isEmpty() && (to == state::inReview || to == state::done)) {
        bool busy = false;
        for (const Task& other : team.tasks)
            busy = busy || (other.owner == task.owner && other.id != task.id &&
                            (other.state == state::todo || other.state == state::inProgress ||
                             other.state == state::blocked));
        if (!busy)
            updates.append(
                {.kind = QStringLiteral("idle_developer"),
                 .text = QStringLiteral("%1 has no task left.").arg(actorName(team, task.owner)),
                 .taskId = task.id,
                 .actionable = true,
                 .data = {{QStringLiteral("session_id"), task.owner}}});
    }
}

TeamResult TeamCoordinator::moveTask(const qint64 teamId, const QString& taskId, const QString& to,
                                     const QString& actor, const QString& note) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task)
        return {.error = taskNotFound(taskId)};
    const QString role = actorRole(*team, actor);
    const bool owner = !task->owner.isEmpty() && task->owner == actor;
    const bool daemon = role == QStringLiteral("daemon");
    const QString from = task->state;
    const auto invalid = [&](const QString& why) {
        return TeamResult{
            .error = refusal(
                QStringLiteral("invalid_transition"),
                QStringLiteral("%1 cannot move from %2 to %3: %4").arg(taskId, from, to, why))};
    };
    QList<Update> updates;
    QString reason = note.trimmed();
    if (to == state::inProgress) {
        if (!owner && !daemon)
            return {.error =
                        refusal(QStringLiteral("not_owner"),
                                QStringLiteral("Only the owner of %1 starts it.").arg(taskId))};
        if (from != state::todo && from != state::blocked && !(daemon && from == state::inReview))
            return invalid(QStringLiteral("only a task in To do or Blocked starts."));
        for (const QString& dependency : task->dependsOn) {
            const Task* other = team->task(dependency);
            if (other && other->state != state::done)
                return {.error = refusal(QStringLiteral("dependencies_open"),
                                         QStringLiteral("%1 waits for %2, which is not done.")
                                             .arg(taskId, dependency))};
        }
        for (const Task& other : team->tasks)
            if (other.id != taskId && other.owner == task->owner &&
                other.state == state::inProgress)
                return invalid(QStringLiteral("finish %1 first; a developer works on one task.")
                                   .arg(other.id));
        task->question.clear();
        task->resumeState.clear();
    } else if (to == state::inReview) {
        if (!owner && !daemon)
            return {.error = refusal(
                        QStringLiteral("not_owner"),
                        QStringLiteral("Only the owner of %1 hands it to review.").arg(taskId))};
        if (from != state::inProgress)
            return invalid(QStringLiteral("only a task In progress goes to review."));
        const auto dirty = host_->hasUncommittedWork(task->worktree);
        if (!dirty)
            return {.error = refusal(
                        QStringLiteral("uncommitted_work"),
                        QStringLiteral("The worktree %1 could not be read.").arg(task->worktree))};
        if (*dirty)
            return {.error = refusal(QStringLiteral("uncommitted_work"),
                                     QStringLiteral("Commit your work in %1 before handing %2 to "
                                                    "review; uncommitted changes are not reviewed.")
                                         .arg(task->worktree, taskId))};
        task->headCommit = host_->taskCommit(*team, *task);
    } else if (to == state::blocked) {
        if (!owner && !daemon && !leaderOrUser(role))
            return {.error =
                        refusal(QStringLiteral("not_owner"),
                                QStringLiteral("Only the owner of %1 blocks it.").arg(taskId))};
        if (from != state::todo && from != state::inProgress &&
            !(daemon && from == state::inReview))
            return invalid(QStringLiteral("only a task To do or In progress is blocked."));
        if (owner && reason.isEmpty())
            return {.error = refusal(QStringLiteral("invalid_payload"),
                                     QStringLiteral("Say what blocks the task: the question for "
                                                    "the leader."))};
        task->resumeState = from == state::inReview ? state::inProgress : from;
        // The actionable state update carries the question to the leader's digest.
        task->question = reason.left(4000);
    } else if (to == state::todo) {
        if (!leaderOrUser(role) && !daemon)
            return {.error = forbidden(QStringLiteral("move a task back to To do"),
                                       QStringLiteral("the leader"))};
        if (from == state::done)
            return invalid(QStringLiteral("a done task stays done."));
        if (!task->owner.isEmpty()) {
            addHistory(*task, actor, QStringLiteral("owner"),
                       QStringLiteral("Unassigned by %1.").arg(actorName(*team, actor)),
                       {{QStringLiteral("from"), task->owner}, {QStringLiteral("to"), QString{}}});
            task->owner.clear();
        }
        task->assignmentHeld = false;
        task->question.clear();
    } else if (to == state::ready || to == state::done) {
        if (!daemon)
            return {.error = refusal(QStringLiteral("role_forbidden"),
                                     QStringLiteral("KetPlus marks a task %1: after its review, or "
                                                    "when the integration branch contains it.")
                                         .arg(to))};
    } else {
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("Unknown state %1.").arg(to))};
    }
    applyMove(*team, *task, to, actor, reason.left(500), updates);
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    return {.teamId = teamId};
}

QString TeamCoordinator::assignmentText(const Team& team, const Task& task, const QString& fromName,
                                        const bool handover, const QString& reason) {
    QStringList lines{
        handover
            ? QStringLiteral("Team %1: task %2 \"%3\" was handed over to you by %4. Reason: %5")
                  .arg(team.name, task.id, task.title, fromName, reason)
            : QStringLiteral("Team %1: task %2 \"%3\" is assigned to you by %4.")
                  .arg(team.name, task.id, task.title, fromName),
        QStringLiteral("Branch: %1").arg(task.branch),
        QStringLiteral("Worktree: %1 (your conversation now works there)").arg(task.worktree),
        QStringLiteral("Lane: %1").arg(task.lane.isEmpty() ? QStringLiteral("(none)") : task.lane)};
    if (!task.description.isEmpty())
        lines << QString{} << task.description;
    if (!task.criteria.isEmpty()) {
        lines << QString{} << QStringLiteral("Acceptance criteria:");
        for (int index = 0; index < task.criteria.size(); ++index)
            lines << QStringLiteral("%1. [%2] %3")
                         .arg(index + 1)
                         .arg(task.criteria.at(index).met ? QStringLiteral("x")
                                                          : QStringLiteral(" "))
                         .arg(task.criteria.at(index).text);
    }
    if (handover && !task.history.isEmpty()) {
        lines << QString{} << QStringLiteral("History:");
        const qsizetype first = qMax<qsizetype>(0, task.history.size() - 12);
        for (qsizetype index = first; index < task.history.size(); ++index)
            lines << QStringLiteral("- %1 %2").arg(task.history.at(index).at.left(16),
                                                   task.history.at(index).text.left(300));
    }
    lines << QString{}
          << QStringLiteral("Start it with team_task_update (state in_progress), report through "
                            "the task, and hand it to review (state in_review) once the criteria "
                            "are met and your work is committed.");
    return lines.join(QLatin1Char('\n'));
}

void TeamCoordinator::deliverAssignment(const Team& storedTeam, const Task& storedTask,
                                        const QString& from, const bool handover,
                                        const QString& reason) {
    // Copies: recording a failed move below detaches the stored lists the arguments refer
    // into, and the old ones are freed with `before`.
    const Team team = storedTeam;
    const Task task = storedTask;
    if (const QString error = host_->pointConversationAt(task.owner, task.worktree);
        !error.isEmpty()) {
        Team* stored = mutableTeam(team.id);
        if (stored && stored->task(task.id)) {
            const Team before = *stored;
            addHistory(*stored->task(task.id), QStringLiteral("daemon"), QStringLiteral("worktree"),
                       QStringLiteral("The owner's conversation was not moved to %1: %2")
                           .arg(task.worktree, error));
            (void)commit(*stored, before, QStringLiteral("daemon"), {});
        }
    }
    const Member* sender = team.member(from);
    wake(team, {.teamId = team.id,
                .teamName = team.name,
                .toSession = task.owner,
                .fromSession = sender ? from : QString{},
                .kind = QStringLiteral("assignment"),
                .taskId = task.id,
                .body = assignmentText(team, task, actorName(team, from), handover, reason)});
}

} // namespace ketplus::daemon::teams
