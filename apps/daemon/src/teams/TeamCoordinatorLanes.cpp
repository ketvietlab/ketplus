// ADR 0114 §4: the check outside a lane. After a developer's turn the daemon compares the files
// changed on each of its open tasks with the owner's prefixes and the team's shared paths.
// The flag blocks nothing; it is actionable in the leader's digest and clears itself.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

bool covered(const QString& path, const QStringList& prefixes) {
    return std::any_of(prefixes.cbegin(), prefixes.cend(),
                       [&](const QString& prefix) { return rules::pathUnder(path, prefix); });
}

// The first paths of `files`, sorted, that neither the owner's lane, the team's shared paths
// nor the task's allowed paths (T-38) cover. A developer without prefixes has no lane to leave. A
// flag that lists hundreds of paths is about the base the files were compared with, not about one
// lane; the first maximumOutsidePaths of them are enough to act on.
QStringList outsidePaths(const Team& team, const Task& task, const QStringList& files) {
    const Member* member = team.member(task.owner);
    if (!member || member->paths.isEmpty())
        return {};
    QStringList outside;
    for (const QString& file : files)
        if (!covered(file, member->paths) && !covered(file, team.sharedPaths) &&
            !covered(file, task.allowedPaths))
            outside.append(file);
    outside.removeDuplicates();
    outside.sort();
    if (outside.size() > maximumOutsidePaths)
        outside.resize(maximumOutsidePaths);
    return outside;
}

// At most a screenful of paths for a message; the rest is a count.
QString listed(const QStringList& paths) {
    constexpr qsizetype shown = 20;
    if (paths.size() <= shown)
        return paths.join(QStringLiteral(", "));
    return QStringLiteral("%1 and %2 more")
        .arg(paths.first(shown).join(QStringLiteral(", ")))
        .arg(paths.size() - shown);
}

bool open(const Task& task) {
    return task.state == state::inProgress || task.state == state::inReview ||
           task.state == state::blocked;
}

} // namespace

bool TeamCoordinator::flagOutside(Team& team, Task& task, const QStringList& outside,
                                  const QString& why, QList<Update>& updates) {
    if (outside == task.outsideLane)
        return false;
    QStringList added;
    for (const QString& path : outside)
        if (!task.outsideLane.contains(path))
            added.append(path);
    const QJsonObject data{{QStringLiteral("paths"), QJsonArray::fromStringList(outside)},
                           {QStringLiteral("added"), QJsonArray::fromStringList(added)}};
    task.outsideLane = outside;
    const Member* owner = team.member(task.owner);
    const QString name = owner ? owner->name : task.owner;
    if (outside.isEmpty()) {
        addHistory(task, QStringLiteral("daemon"), QStringLiteral("outside_lane"),
                   QStringLiteral("Back inside its lane: %1.").arg(why), data);
        updates.append(
            {.kind = QStringLiteral("outside_lane_cleared"),
             .text = QStringLiteral("%1 is back inside %2's lane (%3).").arg(task.id, name, why),
             .taskId = task.id,
             .data = data});
        return true;
    }
    const QString text = QStringLiteral("Outside its lane: %1").arg(listed(outside));
    addHistory(task, QStringLiteral("daemon"), QStringLiteral("outside_lane"), text, data);
    // Only a path that was not flagged before needs the leader.
    updates.append({.kind = QStringLiteral("outside_lane"),
                    .text = QStringLiteral("%1 by %2 changes files outside its lane: %3")
                                .arg(task.id, name, listed(outside)),
                    .taskId = task.id,
                    .actionable = !added.isEmpty(),
                    .data = data});
    return true;
}

void TeamCoordinator::checkLanes(const qint64 teamId, const QString& developer) {
    const Team* team = this->team(teamId);
    if (!team)
        return;
    QList<Task> mine;
    for (const Task& task : team->tasks)
        if (task.owner == developer && open(task) && !task.worktree.isEmpty())
            mine.append(task);
    if (mine.isEmpty())
        return;
    const QString check = QStringLiteral("%1/%2").arg(teamId).arg(developer);
    if (laneChecks_.contains(check)) {
        laneChecksAgain_.insert(check);
        return;
    }
    laneChecks_.insert(check);
    host_->askTasks(
        *team, mine, TaskQuestion::Lane,
        [self = QPointer<TeamCoordinator>(this), teamId, developer, check, mine,
         repository = team->repository,
         integration = team->integrationBranch](const QHash<QString, TaskFacts>& facts) {
            if (!self)
                return;
            self->laneChecks_.remove(check);
            bool again = self->laneChecksAgain_.remove(check);
            // The paths were compared with the integration branch the team had.
            // An answer against another one is dropped and asked again.
            const Team* now = self->team(teamId);
            if (now && now->repository == repository && now->integrationBranch == integration)
                self->applyLaneFacts(teamId, developer, mine, facts);
            else
                again = now != nullptr;
            if (again)
                self->checkLanes(teamId, developer);
        });
}

void TeamCoordinator::applyLaneFacts(const qint64 teamId, const QString& developer,
                                     const QList<Task>& asked,
                                     const QHash<QString, TaskFacts>& facts) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    QHash<QString, QStringList> found;
    for (const Task& was : asked) {
        const Task* task = std::as_const(*team).task(was.id);
        const auto fact = facts.constFind(was.id);
        // The lanes are read as they are now; a task that changed hands or worktree
        // meanwhile is checked after its owner's next turn.
        if (!task || fact == facts.cend() || !fact->changed || task->owner != developer ||
            !open(*task) || task->worktree != was.worktree || task->branch != was.branch ||
            task->baseCommit != was.baseCommit)
            continue;
        found.insert(task->id, outsidePaths(*team, *task, *fact->changed));
    }
    if (found.isEmpty())
        return;
    team = mutableTeam(teamId);
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    QList<Update> updates;
    bool changed = false;
    for (auto it = found.cbegin(); it != found.cend(); ++it)
        if (Task* task = team->task(it.key()))
            changed |= flagOutside(*team, *task, it.value(),
                                   QStringLiteral("the files were reverted"), updates);
    if (changed && commit(*team, before, QStringLiteral("daemon"), updates))
        qWarning("Unable to record the lane check of %s.", qUtf8Printable(developer));
}

void TeamCoordinator::recheckLanes(Team& team, QList<Update>& updates, const QString& why) {
    for (Task& task : team.tasks) {
        if (task.outsideLane.isEmpty())
            continue;
        flagOutside(team, task, outsidePaths(team, task, task.outsideLane),
                    why.isEmpty() ? QStringLiteral("a lane change covers the files") : why,
                    updates);
    }
}

} // namespace ketplus::daemon::teams
