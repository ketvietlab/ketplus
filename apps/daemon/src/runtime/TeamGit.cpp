// ADR 0114: the questions a team asks git. Nothing here reads the session runtime.
#include "runtime/TeamGit.h"

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
