// ADR 0114: the questions a team asks git. Nothing here reads the session runtime.
#include "runtime/TeamGit.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>

namespace ketplus::daemon::team_git {

std::optional<QString> output(const QString& directory, const QStringList& arguments) {
    if (directory.isEmpty())
        return std::nullopt;
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("--no-optional-locks")} + arguments);
    if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
        return std::nullopt;
    return QString::fromUtf8(process.readAllStandardOutput());
}

QString taskCommit(const teams::Team& team, const teams::Task& task) {
    if (task.branch.isEmpty())
        return {};
    // The branch itself, not a worktree's HEAD: a worktree can be detached or on another
    // branch, and the branch can move without it.
    return output(team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                    QStringLiteral("--quiet"), QStringLiteral("--end-of-options"),
                                    QStringLiteral("refs/heads/%1^{commit}").arg(task.branch)})
        .value_or(QString{})
        .trimmed();
}

std::optional<QStringList> changedPaths(const teams::Team& team, const teams::Task& task) {
    if (task.worktree.isEmpty() || !QFileInfo(task.worktree).isDir())
        return std::nullopt;
    const QString base =
        team.integrationBranch.isEmpty() ? task.baseCommit : team.integrationBranch;
    // Committed on the branch since it left its base, then what is not committed yet.
    const auto committed =
        output(task.worktree, {QStringLiteral("diff"), QStringLiteral("--name-only"),
                               QStringLiteral("%1...HEAD").arg(base)});
    const auto pending =
        output(task.worktree,
               {QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
                QStringLiteral("-uall"), QStringLiteral("--no-renames")});
    if (!committed || !pending)
        return std::nullopt;
    QStringList paths = committed->split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& entry : pending->split(QChar(u'\0'), Qt::SkipEmptyParts))
        if (entry.size() > 3)
            paths.append(entry.mid(3));
    paths.removeDuplicates();
    return paths;
}

std::optional<bool> isMerged(const teams::Team& team, const teams::Task& task) {
    if (team.integrationBranch.isEmpty() || task.branch.isEmpty())
        return std::nullopt;
    // Exit 0: an ancestor; 1: not one; anything else: git could not tell.
    QProcess process;
    process.setWorkingDirectory(team.repository);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("--no-optional-locks"), QStringLiteral("merge-base"),
                   QStringLiteral("--is-ancestor"), task.branch, team.integrationBranch});
    if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit)
        return std::nullopt;
    if (process.exitCode() == 0 || process.exitCode() == 1)
        return process.exitCode() == 0;
    return std::nullopt;
}

std::optional<teams::TaskDiff> taskDiff(const teams::Team& team, const teams::Task& task) {
    const QString directory = task.worktree.isEmpty() ? team.repository : task.worktree;
    // The branch against the integration branch it was cut from (ADR 0114 §2).
    const QString range = QStringLiteral("%1...%2").arg(
        team.integrationBranch.isEmpty() ? task.baseCommit : team.integrationBranch, task.branch);
    const auto diff = output(directory, {QStringLiteral("diff"), QStringLiteral("--no-color"),
                                         QStringLiteral("--no-ext-diff"), range});
    const auto names =
        output(directory, {QStringLiteral("diff"), QStringLiteral("--name-only"), range});
    if (!diff || !names)
        return std::nullopt;
    return teams::TaskDiff{.diff = *diff,
                           .files = names->split(QLatin1Char('\n'), Qt::SkipEmptyParts)};
}

// ADR 0114, open point 1: `<repository>-team-<id>-t-<n>` beside the repository, as the
// worktrees of ADR 0111 are, on branch `team/<id>/t-<n>` from the integration branch.
teams::WorktreeAnswer createWorktree(const teams::Team& team, const teams::Task& task) {
    const QString base =
        team.integrationBranch.isEmpty() ? QStringLiteral("HEAD") : team.integrationBranch;
    const auto commit =
        output(team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                 base + QStringLiteral("^{commit}")});
    if (!commit)
        return {.worktree = std::nullopt,
                .error = QStringLiteral("The integration branch %1 does not exist in %2.")
                             .arg(base, team.repository)};
    const QString slug = task.id.toLower();
    const QString branch = QStringLiteral("team/%1/%2").arg(team.id).arg(slug);
    const QFileInfo repository(team.repository);
    const QString path = QDir::cleanPath(repository.dir().filePath(
        QStringLiteral("%1-team-%2-%3").arg(repository.fileName()).arg(team.id).arg(slug)));
    if (QFileInfo::exists(path))
        return {.worktree = std::nullopt,
                .error = QStringLiteral("The worktree path %1 already exists.").arg(path)};
    if (!output(team.repository,
                {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-b"), branch,
                 QStringLiteral("--"), path, commit->trimmed()}))
        return {
            .worktree = std::nullopt,
            .error =
                QStringLiteral("git could not create the worktree %1 on %2.").arg(path, branch)};
    return {.worktree = teams::TaskWorktree{.branch = branch,
                                            .worktree = QFileInfo(path).canonicalFilePath(),
                                            .baseCommit = commit->trimmed()},
            .error = {}};
}

std::optional<bool> hasUncommittedWork(const QString& worktree) {
    const auto status = output(worktree, {QStringLiteral("status"), QStringLiteral("--porcelain")});
    if (!status)
        return std::nullopt;
    return !status->trimmed().isEmpty();
}

teams::HandoffFacts handoff(const teams::Team& team, const teams::Task& task) {
    return {.worktree = task.worktree,
            .branch = task.branch,
            .dirty = hasUncommittedWork(task.worktree),
            .head = taskCommit(team, task)};
}

QHash<QString, teams::TaskFacts> facts(const teams::Team& team, const QList<teams::Task>& tasks,
                                       const teams::TaskQuestion question) {
    QHash<QString, teams::TaskFacts> answers;
    for (const teams::Task& task : tasks)
        answers.insert(task.id, question == teams::TaskQuestion::Merge
                                    ? teams::TaskFacts{.head = taskCommit(team, task),
                                                       .merged = isMerged(team, task),
                                                       .changed = std::nullopt}
                                    : teams::TaskFacts{.head = {},
                                                       .merged = std::nullopt,
                                                       .changed = changedPaths(team, task)});
    return answers;
}

} // namespace ketplus::daemon::team_git
