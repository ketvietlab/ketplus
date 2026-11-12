// T-38: paths any lane may change. A team names them as its shared paths, a task as its own
// allowed paths: contracts, snapshots, fixtures or tests a task has to update. A change there
// is never flagged outside a lane (ADR 0114 §4).
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

QString listed(const QStringList& paths) {
    return paths.isEmpty() ? QStringLiteral("none") : paths.join(QStringLiteral(", "));
}

} // namespace

TeamResult TeamCoordinator::allowPaths(const qint64 teamId, const QString& taskId,
                                       const QStringList& paths, const QString& reason,
                                       const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = TeamError{QStringLiteral("team_not_found"),
                                   QStringLiteral("There is no team %1.").arg(teamId)}};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Task* task = taskId.isEmpty() ? nullptr : team->task(taskId);
    if (!taskId.isEmpty() && !task)
        return {.error = TeamError{QStringLiteral("task_not_found"),
                                   QStringLiteral("The team has no task %1.").arg(taskId)}};
    if (task && task->state == state::done)
        return {.error = TeamError{QStringLiteral("invalid_transition"),
                                   QStringLiteral("%1 is done.").arg(taskId)}};
    const auto normalised = rules::normalisedPaths(paths);
    if (!normalised)
        return {.error = TeamError{
                    QStringLiteral("invalid_payload"),
                    QStringLiteral("Allowed paths are relative to the repository root.")}};
    QStringList& target = task ? task->allowedPaths : team->sharedPaths;
    if (*normalised == target)
        return {.teamId = teamId};
    const QStringList old = target;
    target = *normalised;
    const QString why = reason.trimmed().left(1000);
    const QString text =
        (task ? QStringLiteral("Allowed paths of %1: %2 → %3.")
                    .arg(task->id, listed(old), listed(*normalised))
              : QStringLiteral("Shared paths: %1 → %2.").arg(listed(old), listed(*normalised))) +
        (why.isEmpty() ? QString{} : QStringLiteral(" Reason: ") + why);
    const QJsonObject data{{QStringLiteral("from"), QJsonArray::fromStringList(old)},
                           {QStringLiteral("to"), QJsonArray::fromStringList(*normalised)},
                           {QStringLiteral("reason"), why}};
    if (task)
        addHistory(*task, actor, QStringLiteral("allowed_paths"), text, data);
    QList<Update> updates{
        {.kind = task ? QStringLiteral("allowed_paths") : QStringLiteral("shared_paths"),
         .text = text,
         .taskId = task ? task->id : QString{},
         .data = data}};
    recheckLanes(*team, updates, QStringLiteral("allowed paths cover the files"));
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    return {.teamId = teamId};
}

} // namespace ketplus::daemon::teams
