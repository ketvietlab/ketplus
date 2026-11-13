// T-90 (ADR 0121): the repository a team works in can change, and a task can work in a
// repository of its own. A task that already has a branch keeps the repository and the
// integration branch it was started in, so its worktree, merge check, pull request and
// cleanup keep reading the place its work is. Nothing on disk moves.
#include "teams/TeamCoordinator.h"

#include <QJsonArray>

namespace ketplus::daemon::teams {

namespace {

TeamError invalidRepository(const QString& message) {
    return TeamError{QStringLiteral("invalid_repository"), message};
}

QString branchName(const QString& branch) {
    return branch.isEmpty() ? QStringLiteral("HEAD") : branch;
}

} // namespace

std::optional<TeamError> TeamCoordinator::changeRepository(Team& team, const QJsonObject& changes,
                                                           const QString& actor,
                                                           QList<Update>& updates) {
    const QString path = changes.value(QStringLiteral("repository")).toString().trimmed();
    if (path.isEmpty())
        return invalidRepository(QStringLiteral("Name the folder of the repository."));
    const QString branch =
        changes.contains(QStringLiteral("integration_branch"))
            ? changes.value(QStringLiteral("integration_branch")).toString().trimmed()
            : team.integrationBranch;
    if (auto invalid = rules::validBranch(branch))
        return invalid;
    // Lanes and merges are checked against it: a team that has one changes it, not drops it.
    if (branch.isEmpty() && !team.integrationBranch.isEmpty())
        return TeamError{QStringLiteral("invalid_payload"),
                         QStringLiteral("The integration branch can be changed, not removed.")};
    const QString reason = changes.value(QStringLiteral("reason")).toString().trimmed();
    if (reason.size() > 2000)
        return TeamError{QStringLiteral("invalid_payload"),
                         QStringLiteral("The reason is longer than 2,000 characters.")};
    const RepositoryCheck checked = host_->checkRepository(path, branch);
    if (!checked.error.isEmpty())
        return invalidRepository(checked.error);
    if (checked.repository == team.repository && branch == team.integrationBranch)
        return std::nullopt;

    // Every task with a branch keeps where it was started, done ones too: their worktrees are
    // still cleaned up in that repository.
    QJsonArray kept;
    for (Task& task : team.tasks) {
        if (task.branch.isEmpty() || !task.repository.isEmpty())
            continue;
        task.repository = team.repository;
        task.integrationBranch = team.integrationBranch;
        addHistory(
            task, actor, QStringLiteral("repository"),
            QStringLiteral("Keeps working in %1 against %2: the team moved to %3.")
                .arg(task.repository, branchName(task.integrationBranch), checked.repository),
            {{QStringLiteral("repository"), task.repository},
             {QStringLiteral("integration_branch"), task.integrationBranch}});
        kept.append(task.id);
    }
    QString text = QStringLiteral("Repository: %1 against %2 (was %3 against %4).")
                       .arg(checked.repository, branchName(branch), team.repository,
                            branchName(team.integrationBranch));
    if (!kept.isEmpty())
        text += QStringLiteral(" %1 started task(s) keep the old one.").arg(kept.size());
    if (!reason.isEmpty())
        text += QStringLiteral(" Reason: %1").arg(reason);
    updates.append({.kind = QStringLiteral("repository"),
                    .text = text,
                    .data = {{QStringLiteral("from"), team.repository},
                             {QStringLiteral("to"), checked.repository},
                             {QStringLiteral("from_branch"), team.integrationBranch},
                             {QStringLiteral("to_branch"), branch},
                             {QStringLiteral("reason"), reason},
                             {QStringLiteral("kept"), kept}}});
    team.repository = checked.repository;
    team.integrationBranch = branch;
    return std::nullopt;
}

std::optional<TeamError> TeamCoordinator::taskRepository(const Team& team, const TaskDraft& draft,
                                                         Task& task) const {
    const QString path = draft.repository.trimmed();
    if (path.isEmpty()) {
        if (!draft.integrationBranch.trimmed().isEmpty())
            return TeamError{QStringLiteral("invalid_payload"),
                             QStringLiteral("An integration branch of its own goes with a "
                                            "repository of its own.")};
        return std::nullopt;
    }
    const QString own = draft.integrationBranch.trimmed();
    const QString branch = own.isEmpty() ? team.integrationBranch : own;
    if (auto invalid = rules::validBranch(branch))
        return invalid;
    const RepositoryCheck checked = host_->checkRepository(path, branch);
    if (!checked.error.isEmpty())
        return invalidRepository(checked.error);
    // The team's own repository and branch need no entry: the task follows the team.
    if (checked.repository == team.repository && branch == team.integrationBranch)
        return std::nullopt;
    task.repository = checked.repository;
    task.integrationBranch = branch;
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
