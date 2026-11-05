#include "cloud/WorktreeBranches.h"

#include "cloud/WorktreeRepositories.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumOutputBytes = 1024 * 1024;

struct GitRun {
    QByteArray output;
    bool ok{false};
};

// Git without prompts, pagers or colour, its output capped so a repository with a huge
// number of refs cannot grow it without bound.
GitRun runGit(const QString& directory, const QStringList& arguments, const int timeoutMs) {
    GitRun result;
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty())
        return result;
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(directory);
    process.start(git, QStringList{QStringLiteral("-c"), QStringLiteral("color.ui=false")} +
                           arguments);
    if (!process.waitForStarted(3000))
        return result;
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 100) {
        process.waitForFinished(100);
        result.output += process.readAll();
        if (result.output.size() > maximumOutputBytes)
            break;
        if (process.state() == QProcess::NotRunning) {
            result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
            return result;
        }
    }
    process.kill();
    process.waitForFinished(1000);
    return {};
}

} // namespace

QStringList repositoryCheckouts(const QString& repository) {
    QStringList checkouts;
    if (!QFileInfo(QDir(repository).filePath(QStringLiteral(".git"))).isDir())
        return checkouts;
    checkouts.append(repository);
    // Git lists each linked worktree under <repository>/.git/worktrees/<name>/gitdir, which
    // holds the path of the worktree's .git file.
    const QDir administration(QDir(repository).filePath(QStringLiteral(".git/worktrees")));
    for (const QString& name :
         administration.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile gitdir(administration.filePath(name + QStringLiteral("/gitdir")));
        if (!gitdir.open(QIODevice::ReadOnly))
            continue;
        const QString dotGit = QString::fromUtf8(gitdir.readLine()).trimmed();
        if (!dotGit.endsWith(QStringLiteral("/.git")))
            continue;
        const QString worktree = QDir::cleanPath(dotGit.chopped(5));
        if (QFileInfo(worktree).isDir() && !checkouts.contains(worktree))
            checkouts.append(worktree);
    }
    return checkouts;
}

QString checkoutBranch(const QString& worktree) {
    QFile head(QDir(worktreeGitDirectory(worktree)).filePath(QStringLiteral("HEAD")));
    if (!head.open(QIODevice::ReadOnly))
        return {};
    const QString value = QString::fromUtf8(head.readLine()).trimmed();
    const QString prefix = QStringLiteral("ref: refs/heads/");
    return value.startsWith(prefix) ? value.mid(prefix.size()) : QString();
}

RepositoryBranches readRepositoryBranches(const QString& repository) {
    RepositoryBranches branches;
    const GitRun run = runGit(repository,
                              {QStringLiteral("for-each-ref"), QStringLiteral("--sort=refname"),
                               QStringLiteral("--format=%(refname)%09%(symref)"),
                               QStringLiteral("refs/heads"), QStringLiteral("refs/remotes")},
                              10'000);
    if (!run.ok)
        return branches;
    for (const QByteArray& line : run.output.split('\n')) {
        const QList<QByteArray> fields = line.split('\t');
        // `origin/HEAD` is a symbolic ref, not a branch to start from.
        if (fields.size() != 2 || !fields.at(1).isEmpty())
            continue;
        const QString ref = QString::fromUtf8(fields.at(0));
        if (ref.startsWith(QStringLiteral("refs/heads/")))
            branches.local.append(ref.mid(11));
        else if (ref.startsWith(QStringLiteral("refs/remotes/")))
            branches.remote.append(ref.mid(13));
    }
    branches.ok = true;
    return branches;
}

bool validNewBranchName(const QString& name) {
    static const QRegularExpression forbidden(QStringLiteral(
        R"([\x00-\x20\x7f~^:?*\[\\]|\.\.|@\{|//|^[-/.]|[/.]$|\.lock$|\.lock/|/\.)"));
    return !name.isEmpty() && name.size() <= 200 && name != QStringLiteral("@") &&
           !forbidden.match(name).hasMatch();
}

QString newWorktreePath(const QString& repository, const QString& branch) {
    QString directory = branch.trimmed();
    directory.replace(QLatin1Char('/'), QLatin1Char('-'));
    directory.replace(QLatin1Char('\\'), QLatin1Char('-'));
    const QFileInfo primary(QDir::cleanPath(repository));
    return QDir::cleanPath(
        primary.dir().filePath(primary.fileName() + QLatin1Char('-') + directory));
}

AddedWorktree addWorktree(const QString& repository, const QString& branch, const QString& base) {
    AddedWorktree added;
    if (!validNewBranchName(branch)) {
        added.errorCode = QStringLiteral("invalid_branch_name");
        added.message = QStringLiteral("The branch name is not valid.");
        return added;
    }
    const RepositoryBranches branches = readRepositoryBranches(repository);
    if (!branches.ok) {
        added.errorCode = QStringLiteral("worktree_create_failed");
        added.message = QStringLiteral("Could not read the repository's branches.");
        return added;
    }
    if (branches.local.contains(branch)) {
        added.errorCode = QStringLiteral("branch_exists");
        added.message = QStringLiteral("A branch with this name already exists.");
        return added;
    }
    // The base is a listed name, so it cannot smuggle an option or a revision expression.
    QString baseRef;
    if (branches.local.contains(base))
        baseRef = QStringLiteral("refs/heads/") + base;
    else if (branches.remote.contains(base))
        baseRef = QStringLiteral("refs/remotes/") + base;
    if (baseRef.isEmpty()) {
        added.errorCode = QStringLiteral("base_branch_not_found");
        added.message = QStringLiteral("The branch to start from no longer exists.");
        return added;
    }
    const QString target = newWorktreePath(repository, branch);
    if (QFileInfo::exists(target)) {
        added.errorCode = QStringLiteral("worktree_exists");
        added.message = QStringLiteral("A folder for this worktree already exists.");
        return added;
    }
    const GitRun run = runGit(repository,
                              {QStringLiteral("worktree"), QStringLiteral("add"),
                               QStringLiteral("-b"), branch, QStringLiteral("--"), target, baseRef},
                              30'000);
    if (!run.ok || !QFileInfo(target).isDir()) {
        added.errorCode = QStringLiteral("worktree_create_failed");
        const QString output = QString::fromLocal8Bit(run.output).trimmed();
        added.message = output.isEmpty() ? QStringLiteral("Git could not create the worktree.")
                                         : output.left(500);
        return added;
    }
    added.path = QFileInfo(target).canonicalFilePath();
    return added;
}

} // namespace ketplus::daemon
