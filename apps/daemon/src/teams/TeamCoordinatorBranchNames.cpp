// T-93 (ADR 0123 §9): a new task's branch says what it is, `<type>/<slug>-t<n>`, named by the
// leader or derived from its title, and is refused when that name is already taken or would
// clash with another branch as a folder.
#include "teams/TeamBranchNames.h"
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {

std::optional<TeamError> TeamCoordinator::nameTaskBranch(const Team& team, Task& task,
                                                         const QString& type, const QString& slug,
                                                         bool* unchecked) const {
    const QString chosenType = type.isEmpty() ? branch_names::typeOf(task.title) : type;
    const QString chosenSlug = slug.isEmpty() ? branch_names::slugOf(task.title) : slug;
    if (auto invalid = branch_names::validType(chosenType))
        return invalid;
    if (auto invalid = branch_names::validSlug(chosenSlug))
        return invalid;
    Task named = task;
    named.branchType = chosenType;
    named.branchSlug = chosenSlug;
    const QString branch = branch_names::plannedBranch(team.id, named);
    if (auto invalid = rules::validBranch(branch))
        return invalid;
    const QString repository = taskScope(team, named).repository;
    const auto taken = [&](const QString& where) {
        return TeamError{QStringLiteral("branch_taken"),
                         QStringLiteral("The branch %1 is already %2. Name another with "
                                        "branch_slug.")
                             .arg(branch, where)};
    };
    // A task of any team in the same repository, started or not: its branch is, or will be,
    // the same ref.
    for (const Team& other : teams_) {
        for (const Task& candidate : other.tasks) {
            if (other.id == team.id && candidate.id == task.id)
                continue;
            if (taskScope(other, candidate).repository != repository)
                continue;
            const QString theirs = candidate.branch.isEmpty()
                                       ? branch_names::plannedBranch(other.id, candidate)
                                       : candidate.branch;
            if (branch_names::refsClash(theirs, branch))
                return taken(QStringLiteral("%1's of team %2").arg(candidate.id).arg(other.id));
        }
    }
    // Taken also covers a folder clash (`feat` against `feat/x-t1`), which git refuses.
    const std::optional<bool> inRepository = host_->branchTaken(repository, branch);
    if (inRepository == std::optional<bool>(true))
        return taken(QStringLiteral("in %1 or its origin, or clashes with a branch there as a "
                                    "folder")
                         .arg(repository));
    // The host has not listed the repository's branches yet: the name is checked again, for
    // good, when the worktree is made.
    if (unchecked)
        *unchecked = !inRepository.has_value();
    task.branchType = chosenType;
    task.branchSlug = chosenSlug;
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
