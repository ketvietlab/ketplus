// ADR 0114 §10: the size of a worktree and its removal, off the session runtime's event loop.
#include "runtime/TeamWorktreeRemoval.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace ketplus::daemon::team_git {
namespace {

struct GitAnswer final {
    bool ok{false};
    QString output;
    QString error;
};

GitAnswer git(const QString& directory, const QStringList& arguments, const int timeoutMs) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {.ok = false, .output = {}, .error = QStringLiteral("git did not answer in time.")};
    }
    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {.ok = false,
                .output = {},
                .error = error.isEmpty() ? QStringLiteral("git failed.") : error};
    return {.ok = true, .output = QString::fromUtf8(process.readAllStandardOutput()), .error = {}};
}

} // namespace

// The bytes the folder takes on disk, without following links out of it.
qint64 sizeOnDisk(const QString& folder) {
    qint64 total = 0;
    QDirIterator files(folder, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
#ifdef Q_OS_UNIX
        struct stat status {};
        if (::lstat(QFile::encodeName(path).constData(), &status) == 0) {
            total += static_cast<qint64>(status.st_blocks) * 512;
            continue;
        }
#endif
        total += QFileInfo(path).size();
    }
    return total;
}

// The checks a removal makes, then `git worktree remove` without force and `git worktree
// prune`, run in the main checkout. The error in words, or empty when it was removed.
QString removeWorktree(const QString& repository, const QString& worktree) {
    const QString path = QDir::cleanPath(QFileInfo(worktree).absoluteFilePath());
    if (QFileInfo(QDir(path).filePath(QStringLiteral(".git"))).isDir())
        return QStringLiteral("%1 is a repository's main checkout, which is never removed.").arg(path);
    const QStringList quiet{QStringLiteral("--no-optional-locks")};
    const GitAnswer status = git(path,
                                 quiet + QStringList{QStringLiteral("status"),
                                                     QStringLiteral("--porcelain=v1"), QStringLiteral("-z"),
                                                     QStringLiteral("--untracked-files=all")},
                                 15000);
    if (!status.ok)
        return QStringLiteral("The worktree could not be read: %1").arg(status.error);
    if (const qsizetype changed = status.output.split(QChar(u'\0'), Qt::SkipEmptyParts).size();
        changed > 0)
        return QStringLiteral("The worktree has uncommitted or untracked changes (%1). Commit or "
                              "discard them, then remove it again.")
            .arg(changed == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(changed));
    const GitAnswer unpushed =
        git(path,
            quiet + QStringList{QStringLiteral("rev-list"), QStringLiteral("--count"),
                                QStringLiteral("HEAD"), QStringLiteral("--not"),
                                QStringLiteral("--remotes"), QStringLiteral("--")},
            15000);
    if (!unpushed.ok)
        return QStringLiteral("Its commits could not be compared with the remote branches: %1")
            .arg(unpushed.error);
    if (const int commits = unpushed.output.trimmed().toInt(); commits > 0)
        return QStringLiteral("The worktree has %1 on no remote branch. Push them, then remove it "
                              "again.")
            .arg(commits == 1 ? QStringLiteral("1 commit") : QStringLiteral("%1 commits").arg(commits));
    // An absolute path never reads as an option.
    const GitAnswer removed =
        git(repository, {QStringLiteral("worktree"), QStringLiteral("remove"), path}, 120000);
    if (!removed.ok)
        return QStringLiteral("git worktree remove failed: %1").arg(removed.error);
    const GitAnswer pruned = git(repository, {QStringLiteral("worktree"), QStringLiteral("prune")}, 30000);
    if (!pruned.ok)
        return QStringLiteral("The worktree was removed, but git worktree prune failed: %1")
            .arg(pruned.error);
    return {};
}


} // namespace ketplus::daemon::team_git
