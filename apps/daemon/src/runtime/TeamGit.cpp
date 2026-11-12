// ADR 0114: the questions a team asks git. Nothing here reads the session runtime.
#include "runtime/TeamGit.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QProcess>
#include <QProcessEnvironment>

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

QString repositoryOf(const QString& worktreePath) {
    QProcess process;
    process.setWorkingDirectory(worktreePath);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("--no-optional-locks"), QStringLiteral("rev-parse"),
                   QStringLiteral("--path-format=absolute"), QStringLiteral("--git-common-dir")});
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
        return {};
    const QFileInfo common(QString::fromUtf8(process.readAllStandardOutput()).trimmed());
    // The main worktree holds the common ".git"; a bare repository is its own key.
    return common.fileName() == QStringLiteral(".git")
               ? QFileInfo(common.absolutePath()).canonicalFilePath()
               : common.canonicalFilePath();
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
    if (task.worktree.isEmpty() || !QFileInfo(task.worktree).isDir() || task.branch.isEmpty())
        return std::nullopt;
    // T-38: only the task's own tracked changes. Committed: the task's branch (not whatever
    // the worktree has checked out) since it left its base. With an integration branch, the
    // diff from where the branch left it. Without one the base is the commit the task started
    // from, and a developer who merges the repository's main line into the branch brings in
    // everyone else's files, so only the branch's own commits count.
    const QString branch = QStringLiteral("refs/heads/%1").arg(task.branch);
    const auto committed =
        team.integrationBranch.isEmpty()
            ? output(task.worktree, {QStringLiteral("log"), QStringLiteral("--first-parent"),
                                     QStringLiteral("--no-merges"), QStringLiteral("--format="),
                                     QStringLiteral("--name-only"), QStringLiteral("--no-renames"),
                                     QStringLiteral("-z"), QStringLiteral("--end-of-options"),
                                     QStringLiteral("%1..%2").arg(task.baseCommit, branch)})
            : output(task.worktree,
                     {QStringLiteral("diff"), QStringLiteral("--name-only"), QStringLiteral("-z"),
                      QStringLiteral("--end-of-options"),
                      QStringLiteral("%1...%2").arg(team.integrationBranch, branch)});
    if (!committed)
        return std::nullopt;
    // -z: a path is spelled as it is, not quoted; --end-of-options: a branch is never an option.
    QStringList paths = committed->split(QChar(u'\0'), Qt::SkipEmptyParts);
    // Not committed yet: tracked files only (-uno), so build trees, captures and other
    // untracked or ignored files are never work outside a lane; and only while the worktree
    // has the task's branch checked out, since a worktree at another commit is not its work.
    const auto checkedOut =
        output(task.worktree,
               {QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"), QStringLiteral("HEAD")});
    if (checkedOut && checkedOut->trimmed() == branch) {
        const auto pending =
            output(task.worktree,
                   {QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                    QStringLiteral("-z"), QStringLiteral("-uno"), QStringLiteral("--no-renames")});
        if (!pending)
            return std::nullopt;
        for (const QString& entry : pending->split(QChar(u'\0'), Qt::SkipEmptyParts))
            if (entry.size() > 3)
                paths.append(entry.mid(3));
    }
    paths.removeDuplicates();
    return paths;
}

// Exit 0: an ancestor; 1: not one; anything else: git could not tell.
std::optional<bool> isAncestor(const QString& repository, const QString& commit,
                               const QString& of) {
    QProcess process;
    process.setWorkingDirectory(repository);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("--no-optional-locks"), QStringLiteral("merge-base"),
                   QStringLiteral("--is-ancestor"), QStringLiteral("--end-of-options"), commit,
                   of});
    if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit)
        return std::nullopt;
    if (process.exitCode() == 0 || process.exitCode() == 1)
        return process.exitCode() == 0;
    return std::nullopt;
}

namespace {

QString remoteIntegration(const teams::Team& team) {
    return QStringLiteral("refs/remotes/origin/%1").arg(team.integrationBranch);
}

// ADR 0118: the integration branch as the remote has it, for a merge made there. Updates only
// the remote-tracking branch: the checkout, its branch and its files stay as they are. At most
// once a minute for a repository; offline it fails quietly and the last fetch counts.
void fetchIntegration(const teams::Team& team) {
    static QMutex mutex;
    static QHash<QString, QDateTime> fetched;
    const QString key = team.repository + u'\n' + team.integrationBranch;
    {
        const QMutexLocker lock(&mutex);
        const auto last = fetched.constFind(key);
        if (last != fetched.cend() && last->secsTo(QDateTime::currentDateTimeUtc()) < 60)
            return;
        fetched.insert(key, QDateTime::currentDateTimeUtc());
    }
    if (!output(team.repository,
                {QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin")}))
        return;
    QProcess process;
    process.setWorkingDirectory(team.repository);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    // An ssh remote asks for no passphrase and no host key: it fails at once instead.
    if (!environment.contains(QStringLiteral("GIT_SSH_COMMAND")))
        environment.insert(QStringLiteral("GIT_SSH_COMMAND"),
                           QStringLiteral("ssh -o BatchMode=yes"));
    process.setProcessEnvironment(environment);
    process.start(
        QStringLiteral("git"),
        {QStringLiteral("fetch"), QStringLiteral("--quiet"), QStringLiteral("--no-tags"),
         QStringLiteral("--no-write-fetch-head"), QStringLiteral("origin"),
         QStringLiteral("+refs/heads/%1:%2").arg(team.integrationBranch, remoteIntegration(team))});
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(1000);
    }
}

} // namespace

std::optional<bool> isMerged(const teams::Team& team, const teams::Task& task) {
    if (team.integrationBranch.isEmpty() || task.branch.isEmpty())
        return std::nullopt;
    const auto local = isAncestor(team.repository, task.branch, team.integrationBranch);
    if (local && *local)
        return true;
    // Merged on the remote while the local branch lags (ADR 0118).
    if (output(team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                 QStringLiteral("--quiet"), remoteIntegration(team)})) {
        const auto remote = isAncestor(team.repository, task.branch, remoteIntegration(team));
        if (remote)
            return *remote;
    }
    return local;
}

std::optional<bool> branchExists(const teams::Team& team, const teams::Task& task) {
    if (task.branch.isEmpty())
        return false;
    if (team.repository.isEmpty())
        return std::nullopt;
    QProcess process;
    process.setWorkingDirectory(team.repository);
    process.start(QStringLiteral("git"),
                  {QStringLiteral("--no-optional-locks"), QStringLiteral("show-ref"),
                   QStringLiteral("--verify"), QStringLiteral("--quiet"),
                   QStringLiteral("refs/heads/%1").arg(task.branch)});
    if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit)
        return std::nullopt;
    // 0: there; 1: git answered and there is no such branch; anything else: git failed.
    if (process.exitCode() == 0 || process.exitCode() == 1)
        return process.exitCode() == 0;
    return std::nullopt;
}

std::optional<teams::TaskDiff> taskDiff(const teams::Team& team, const teams::Task& task) {
    const QString directory = task.worktree.isEmpty() ? team.repository : task.worktree;
    // T-38: a branch still at its base has nothing to show; the review reads the work where
    // the hand to review named it, never another commit the worktree has checked out.
    // A deleted branch has nothing to show either.
    if (const QString tip = taskCommit(team, task);
        (!tip.isEmpty() && tip == task.baseCommit) ||
        (tip.isEmpty() && branchExists(team, task) == std::optional<bool>(false)))
        return teams::TaskDiff{.diff = {}, .files = {}, .outsideBranch = true};
    // The branch against the integration branch it was cut from (ADR 0114 §2), up to the
    // commit handed to review (T-38), not whatever the branch points at since.
    const QString range = QStringLiteral("%1...%2").arg(
        team.integrationBranch.isEmpty() ? task.baseCommit : team.integrationBranch,
        task.headCommit.isEmpty() ? task.branch : task.headCommit);
    const auto diff = output(directory, {QStringLiteral("diff"), QStringLiteral("--no-color"),
                                         QStringLiteral("--no-ext-diff"),
                                         QStringLiteral("--end-of-options"), range});
    const auto names =
        output(directory, {QStringLiteral("diff"), QStringLiteral("--name-only"),
                           QStringLiteral("-z"), QStringLiteral("--end-of-options"), range});
    if (!diff || !names)
        return std::nullopt;
    return teams::TaskDiff{.diff = *diff,
                           .files = names->split(QChar(u'\0'), Qt::SkipEmptyParts),
                           .outsideBranch = false};
}

// ADR 0114, open point 1: `<repository>-team-<id>-t-<n>` beside the repository, as the
// worktrees of ADR 0111 are, on branch `team/<id>/t-<n>` from the integration branch.
teams::WorktreeAnswer createWorktree(const teams::Team& team, const teams::Task& task) {
    const QString base =
        team.integrationBranch.isEmpty() ? QStringLiteral("HEAD") : team.integrationBranch;
    const auto commit = output(
        team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                          QStringLiteral("--end-of-options"), base + QStringLiteral("^{commit}")});
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

std::optional<QString> ownHead(const teams::Team& team, const teams::Task& task) {
    // A deleted or never made branch has no work of its own; only a git that cannot answer
    // is not known, and the caller says to try again.
    const auto exists = branchExists(team, task);
    if (!exists)
        return std::nullopt;
    if (!*exists)
        return QString{};
    const auto tip =
        output(team.repository, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                 QStringLiteral("--end-of-options"),
                                 QStringLiteral("refs/heads/%1^{commit}").arg(task.branch)});
    if (!tip || tip->trimmed().isEmpty())
        return std::nullopt;
    const QString head = tip->trimmed();
    // T-38: a branch still at its base has no work of its own.
    if (head == task.baseCommit)
        return QString{};
    // Moved to a commit that does not come after its base, as another task's.
    if (!task.baseCommit.isEmpty()) {
        const auto after = isAncestor(team.repository, task.baseCommit, head);
        if (!after)
            return std::nullopt;
        if (!*after)
            return QString{};
    }
    // Already in the integration branch: nothing on it is the task's to hand in.
    if (!team.integrationBranch.isEmpty()) {
        const auto contained = isAncestor(team.repository, head, team.integrationBranch);
        if (!contained)
            return std::nullopt;
        if (*contained)
            return QString{};
    }
    return head;
}

teams::HandoffFacts handoff(const teams::Team& team, const teams::Task& task) {
    const auto head = ownHead(team, task);
    return {.worktree = task.worktree,
            .branch = task.branch,
            .dirty = hasUncommittedWork(task.worktree),
            .head = head.value_or(QString{}),
            .headRead = head.has_value()};
}

QHash<QString, teams::TaskFacts> facts(const teams::Team& team, const QList<teams::Task>& tasks,
                                       const teams::TaskQuestion question) {
    QHash<QString, teams::TaskFacts> answers;
    if (question == teams::TaskQuestion::Merge && !team.integrationBranch.isEmpty() &&
        !tasks.isEmpty())
        fetchIntegration(team);
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
