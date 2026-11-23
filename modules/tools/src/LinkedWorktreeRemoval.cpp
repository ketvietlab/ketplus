#include "LinkedWorktreeRemoval.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace ketplus::git {
namespace {

struct GitAnswer {
    QByteArray output;
    QString error;
};

GitAnswer runGit(const QString& directory, const QStringList& arguments, int timeoutMs = 15000) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys())
        if (key.startsWith(QStringLiteral("GIT_"), Qt::CaseInsensitive))
            environment.remove(key);
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("--no-optional-locks"), QStringLiteral("-c"),
                              QStringLiteral("core.fsmonitor=false")} +
                      arguments);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {{}, QStringLiteral("Git could not start or did not answer in time.")};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const auto error = QString::fromUtf8(process.readAllStandardError()).trimmed();
        return {{}, error.isEmpty() ? QStringLiteral("Git failed.") : error};
    }
    return {process.readAllStandardOutput(), {}};
}

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
bool inWorktree(const QString& folder, const QString& worktree) {
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    return canonical == worktree || canonical.startsWith(worktree + QLatin1Char('/'));
}
#endif

QString processBlocker(const QString& path) {
    const auto busy = QStringLiteral("A process is using this worktree as its working directory.");
#if defined(Q_OS_LINUX)
    const QDir proc(QStringLiteral("/proc"));
    if (!QFileInfo::exists(proc.filePath(QStringLiteral("self/cwd"))))
        return QStringLiteral("The processes using this worktree could not be checked.");
    const auto processes = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& process : processes) {
        if (!process.front().isDigit())
            continue;
        const auto cwd = QFileInfo(proc.filePath(process + QStringLiteral("/cwd"))).symLinkTarget();
        if (!cwd.isEmpty() && inWorktree(cwd, path))
            return busy;
    }
#elif defined(Q_OS_MACOS)
    QProcess process;
    process.start(QStringLiteral("/usr/sbin/lsof"),
                  {QStringLiteral("-nP"), QStringLiteral("-a"), QStringLiteral("-u"),
                   QString::number(::geteuid()), QStringLiteral("-d"), QStringLiteral("cwd"),
                   QStringLiteral("-F0n")});
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0)
        return QStringLiteral("The processes using this worktree could not be checked.");
    for (const auto& field : process.readAllStandardOutput().split('\0'))
        if (field.startsWith('n') && inWorktree(QFile::decodeName(field.mid(1)), path))
            return busy;
#else
    // Windows prevents removing a directory that a process holds as its working directory.
    Q_UNUSED(path);
    Q_UNUSED(busy);
#endif
    return {};
}

QString markerLine(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32768)
        return {};
    QByteArray line = file.readLine();
    if (line.endsWith('\n'))
        line.chop(1);
    if (line.endsWith('\r'))
        line.chop(1);
    return QFile::decodeName(line);
}

} // namespace

QString linkedWorktreeIdentity(const QString& worktree) {
    const QFileInfo checkout(worktree);
    const QFileInfo marker(QDir(worktree).filePath(QStringLiteral(".git")));
    if (!checkout.isAbsolute() || !checkout.isDir() || checkout.isSymLink() || !marker.isFile() ||
        marker.isSymLink())
        return {};
    const auto pointer = markerLine(marker.absoluteFilePath());
    if (!pointer.startsWith(QStringLiteral("gitdir: ")))
        return {};
    const QFileInfo registration(QDir(worktree).filePath(pointer.mid(8)));
    const QString directory = registration.canonicalFilePath();
    if (directory.isEmpty() || !registration.isDir() ||
        !QFileInfo(QDir(directory).filePath(QStringLiteral("commondir"))).isFile())
        return {};
    const auto backlink = markerLine(QDir(directory).filePath(QStringLiteral("gitdir")));
    if (backlink.isEmpty() || QFileInfo(QDir(directory).filePath(backlink)).canonicalFilePath() !=
                                  marker.canonicalFilePath())
        return {};
    const auto created = registration.birthTime();
    if (!created.isValid())
        return {};
    return directory + QLatin1Char('|') + QString::number(created.toMSecsSinceEpoch());
}

QString removeLinkedWorktree(const QString& repository, const QString& worktree,
                             const QString& expectedIdentity) {
    const QFileInfo candidate(worktree);
    const QString path = candidate.canonicalFilePath();
    const QFileInfo source(repository);
    const QString root = source.canonicalFilePath();
    if (!candidate.isAbsolute() || !candidate.isDir() || candidate.isSymLink() ||
        !source.isAbsolute() || !source.isDir() || root.isEmpty() || path.isEmpty())
        return QStringLiteral("Only an existing linked worktree can be removed.");
    const QFileInfo gitFile(QDir(path).filePath(QStringLiteral(".git")));
    if (path == root || gitFile.isDir())
        return QStringLiteral("%1 is a repository's main checkout, which is never removed.")
            .arg(path);
    if (!gitFile.isFile() || gitFile.isSymLink())
        return QStringLiteral("Only an existing linked worktree can be removed.");

    const auto list = runGit(root, {QStringLiteral("worktree"), QStringLiteral("list"),
                                    QStringLiteral("--porcelain"), QStringLiteral("-z")});
    if (!list.error.isEmpty())
        return QStringLiteral("The repository's worktrees could not be read: %1").arg(list.error);
    const auto entries = list.output.split('\0');
    // Git emits the main checkout first, including when its .git is a file.
    bool main = true;
    QString registeredPath;
    for (const auto& entry : entries) {
        if (!entry.startsWith("worktree "))
            continue;
        const auto entryPath = QFile::decodeName(entry.mid(9));
        if (QFileInfo(entryPath).canonicalFilePath() == path) {
            if (main)
                return QStringLiteral("%1 is a repository's main checkout, which is never removed.")
                    .arg(path);
            registeredPath = entryPath;
            break;
        }
        main = false;
    }
    if (registeredPath.isEmpty())
        return QStringLiteral("The path is not a linked worktree of this repository.");

    const auto worktreeCommon =
        runGit(path, {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"),
                      QStringLiteral("--git-common-dir")});
    const auto sourceCommon =
        runGit(root, {QStringLiteral("rev-parse"), QStringLiteral("--path-format=absolute"),
                      QStringLiteral("--git-common-dir")});
    if (!worktreeCommon.error.isEmpty() || !sourceCommon.error.isEmpty() ||
        worktreeCommon.output != sourceCommon.output)
        return QStringLiteral("The path no longer belongs to the selected repository.");

    const auto index =
        runGit(path, {QStringLiteral("ls-files"), QStringLiteral("-v"), QStringLiteral("-z")});
    if (!index.error.isEmpty())
        return QStringLiteral("The worktree index could not be read: %1").arg(index.error);
    for (const auto& entry : index.output.split('\0')) {
        if (!entry.isEmpty() &&
            (entry.front() == 'S' || (entry.front() >= 'a' && entry.front() <= 'z')))
            return QStringLiteral("The worktree index excludes files from change detection.");
    }

    const auto status = runGit(
        path, {QStringLiteral("status"), QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
               QStringLiteral("--no-renames"), QStringLiteral("--untracked-files=all"),
               QStringLiteral("--ignored=matching"), QStringLiteral("--ignore-submodules=none")});
    if (!status.error.isEmpty())
        return QStringLiteral("The worktree could not be read: %1").arg(status.error);
    if (const auto changed = status.output.count('\0'); changed > 0)
        return QStringLiteral("The worktree has uncommitted or untracked changes (%1), which "
                              "can include ignored files.")
            .arg(changed == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(changed));

    const auto unpushed =
        runGit(path, {QStringLiteral("rev-list"), QStringLiteral("--count"), QStringLiteral("HEAD"),
                      QStringLiteral("--not"), QStringLiteral("--remotes"), QStringLiteral("--")});
    bool validCount = false;
    const auto commits = unpushed.output.trimmed().toULongLong(&validCount);
    if (!unpushed.error.isEmpty() || !validCount)
        return QStringLiteral("Its commits could not be compared with the remote branches: %1")
            .arg(unpushed.error);
    if (commits != 0)
        return QStringLiteral("The worktree has %1 on no remote branch. Push them, then remove it "
                              "again.")
            .arg(commits == 1 ? QStringLiteral("1 commit")
                              : QStringLiteral("%1 commits").arg(commits));

    if (const auto blocker = processBlocker(path); !blocker.isEmpty())
        return blocker;
    if (!expectedIdentity.isEmpty() && linkedWorktreeIdentity(worktree) != expectedIdentity)
        return QStringLiteral("The worktree changed since it was selected.");
    const auto removed = runGit(root,
                                {QStringLiteral("worktree"), QStringLiteral("remove"),
                                 QStringLiteral("--"), registeredPath},
                                120000);
    if (!removed.error.isEmpty())
        return QStringLiteral("git worktree remove failed: %1").arg(removed.error);
    const auto pruned = runGit(root, {QStringLiteral("worktree"), QStringLiteral("prune")}, 30000);
    // The directory is gone even if Git could not prune other stale registrations.
    if (!pruned.error.isEmpty())
        qWarning().noquote() << QStringLiteral(
                                    "The worktree was removed, but git worktree prune failed:")
                             << pruned.error;
    return {};
}

} // namespace ketplus::git
