#include "snapshot/WorkspaceStore.h"

#include "snapshot/WorkspaceApply.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <sys/stat.h>
#include <unistd.h>

namespace ketplus::runner::snapshot {
namespace {

QString oneLine(const QByteArray& output) { return QString::fromUtf8(output).trimmed(); }

bool hasMirror(const QString& mirror) {
    return QFileInfo::exists(QDir(mirror).filePath(QStringLiteral("HEAD")));
}

enum class Kind { Missing, Link, Directory, Other };

Kind kindOf(const QString& path) {
    struct stat status{};
    if (::lstat(QFile::encodeName(path).constData(), &status) != 0)
        return Kind::Missing;
    if (S_ISLNK(status.st_mode))
        return Kind::Link;
    return S_ISDIR(status.st_mode) ? Kind::Directory : Kind::Other;
}

QStringList entriesOf(const QString& directory) {
    return QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                     QDir::NoDotAndDotDot);
}

// Removes `path` without following any link: a link or file is unlinked, a directory is
// emptied the same way and then removed.
bool removeTree(const QString& path) {
    switch (kindOf(path)) {
    case Kind::Missing:
        return true;
    case Kind::Link:
    case Kind::Other:
        return ::unlink(QFile::encodeName(path).constData()) == 0;
    case Kind::Directory:
        break;
    }
    bool ok = true;
    for (const QString& name : entriesOf(path))
        ok = removeTree(path + QLatin1Char('/') + name) && ok;
    return ok && ::rmdir(QFile::encodeName(path).constData()) == 0;
}

// Removes `relative` below `root` unless one of its parent directories is a link.
bool removeBelow(const QString& root, const QString& relative) {
    const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return true;
    QString current = root;
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        current += QLatin1Char('/') + parts.at(i);
        if (kindOf(current) != Kind::Directory)
            return true; // gone, or reached through a link: nothing of the workspace to delete
    }
    return removeTree(current + QLatin1Char('/') + parts.last());
}

// Deletes every entry no snapshot can hold (`.git` and its look-alikes), at any depth.
bool removeGitEntries(const QString& directory) {
    bool ok = true;
    for (const QString& name : entriesOf(directory)) {
        const QString path = directory + QLatin1Char('/') + name;
        if (!isSafeSnapshotPath(name))
            ok = removeTree(path) && ok;
        else if (kindOf(path) == Kind::Directory)
            ok = removeGitEntries(path) && ok;
    }
    return ok;
}

QString canonicalOrPlanned(const QString& path) {
    const QFileInfo info(path);
    if (info.exists())
        return info.canonicalFilePath();
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return parent.isEmpty() ? QDir::cleanPath(path) : parent + QLatin1Char('/') + info.fileName();
}

} // namespace

WorkspaceState workspaceState(const QString& mirrorPath, const QString& canonicalWorkspace) {
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(canonicalWorkspace.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(32));
    return {QDir(QDir::cleanPath(mirrorPath))
                .filePath(QStringLiteral("ketplus-workspaces/") + key + QStringLiteral(".index")),
            QStringLiteral("refs/ketplus/workspaces/") + key};
}

Result<bool> mirrorHasCommit(const QString& mirrorPath, const QString& commit,
                             const GitTool& tool) {
    if (!isObjectId(commit))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("malformed snapshot id")};
    if (!hasMirror(mirrorPath))
        return false;
    GitRun check;
    check.gitDir = QDir::cleanPath(mirrorPath);
    check.arguments = {QStringLiteral("cat-file"), QStringLiteral("-t"), commit};
    const auto type = SealedGit(tool).run(check);
    if (!type)
        return type.error().code == ErrorCode::GitFailed ? Result<bool>(false)
                                                         : Result<bool>(type.error());
    return oneLine(type.value().out) == QStringLiteral("commit");
}

Result<bool> mirrorHasSnapshot(const QString& mirrorPath, const QString& commit,
                               const GitTool& tool) {
    const Result<bool> present = mirrorHasCommit(mirrorPath, commit, tool);
    if (!present || !present.value())
        return present;
    GitRun check;
    check.gitDir = QDir::cleanPath(mirrorPath);
    check.arguments = {QStringLiteral("rev-list"), QStringLiteral("--objects"),
                       QStringLiteral("--missing=error"), commit};
    check.outputFile = QProcess::nullDevice();
    const auto connected = SealedGit(tool).run(check);
    if (!connected)
        return connected.error().code == ErrorCode::GitFailed ? Result<bool>(false)
                                                              : Result<bool>(connected.error());
    return true;
}

std::optional<Error> cleanWorkspace(const SealedGit& git, const QString& mirrorPath,
                                    const QString& canonicalWorkspace) {
    GitRun list;
    list.gitDir = QDir::cleanPath(mirrorPath);
    list.workTree = canonicalWorkspace;
    list.indexFile = workspaceState(mirrorPath, canonicalWorkspace).indexFile;
    list.arguments = {QStringLiteral("ls-files"), QStringLiteral("-z"), QStringLiteral("--others"),
                      QStringLiteral("--directory")};
    const auto untracked = git.run(list);
    if (!untracked)
        return untracked.error();
    bool ok = true;
    for (const QByteArray& raw : untracked.value().out.split('\0'))
        if (!raw.isEmpty())
            ok = removeBelow(canonicalWorkspace, QString::fromUtf8(raw)) && ok;
    ok = removeGitEntries(canonicalWorkspace) && ok;
    if (!ok)
        return Error{ErrorCode::Io, QStringLiteral("cannot clean the workspace")};
    return std::nullopt;
}

Result<bool> removeWorkspace(const QString& mirrorPath, const QString& workspacePath,
                             const GitTool& tool) {
    if (!QDir::isAbsolutePath(mirrorPath) || !QDir::isAbsolutePath(workspacePath))
        return Error{ErrorCode::InvalidArgument, QStringLiteral("paths must be absolute")};
    const QString canonical = canonicalOrPlanned(workspacePath);
    const WorkspaceState state = workspaceState(mirrorPath, canonical);
    if (!removeTree(canonical))
        return Error{ErrorCode::Io, QStringLiteral("cannot remove the workspace")};
    for (const QString& file : {state.indexFile, state.indexFile + QStringLiteral(".lock")})
        if (!removeTree(file))
            return Error{ErrorCode::Io, QStringLiteral("cannot remove the workspace index")};
    if (hasMirror(mirrorPath)) {
        GitRun drop;
        drop.gitDir = QDir::cleanPath(mirrorPath);
        drop.arguments = {QStringLiteral("update-ref"), QStringLiteral("-d"), state.ref};
        if (const auto dropped = SealedGit(tool).run(drop);
            !dropped && dropped.error().code != ErrorCode::GitFailed)
            return dropped.error();
    }
    return true;
}

} // namespace ketplus::runner::snapshot
