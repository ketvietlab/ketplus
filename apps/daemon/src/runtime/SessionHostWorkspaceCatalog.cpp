#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
namespace ketplus::daemon {
namespace {
// The repository a registered checkout belongs to, read from its `.git` without running Git: a
// linked worktree's `.git` file names its gitdir, whose `commondir` leads to the main checkout's
// `.git`. ADR 0119: Desktop groups checkouts by this on every route, so a repository with linked
// worktrees is one sidebar row. A checkout whose links cannot be read is its own repository.
QString repositoryPathOf(const QString& registered) {
    // Canonical on every branch, so a checkout registered through a symlink and a linked
    // worktree resolving to it name the same repository.
    const QString canonical = QFileInfo(registered).canonicalFilePath();
    const QString checkout = canonical.isEmpty() ? registered : canonical;
    const QFileInfo dotGit(QDir(checkout).filePath(QStringLiteral(".git")));
    if (!dotGit.isFile())
        return checkout;
    QFile link(dotGit.filePath());
    if (!link.open(QIODevice::ReadOnly) || link.size() > 4096)
        return checkout;
    const QString line = QString::fromUtf8(link.readLine()).trimmed();
    if (!line.startsWith(QLatin1String("gitdir:")))
        return checkout;
    const QString gitDir = QDir(checkout).absoluteFilePath(line.mid(7).trimmed());
    QFile common(QDir(gitDir).filePath(QStringLiteral("commondir")));
    if (!common.open(QIODevice::ReadOnly) || common.size() > 4096)
        return checkout;
    const QFileInfo commonDir(
        QDir(gitDir).absoluteFilePath(QString::fromUtf8(common.readLine()).trimmed()));
    const QString commonPath = commonDir.canonicalFilePath();
    if (commonPath.isEmpty())
        return checkout;
    // A bare repository's common dir is the repository itself; otherwise it is `<root>/.git`.
    return QFileInfo(commonPath).fileName() == QLatin1String(".git") ? QFileInfo(commonPath).path()
                                                                     : commonPath;
}
} // namespace
QJsonObject SessionHost::workspaceCatalog(const QString& type, const QJsonObject& payload) {
    const auto fail = [](const char* code, const char* message) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", message}};
    };
    if (type == "workspace.list") {
        QJsonArray entries;
        for (const auto& path : journal_->worktreePaths())
            entries.append(QJsonObject{{"path", path},
                                       {"available", QFileInfo(path).isDir()},
                                       {"repository_path", repositoryPathOf(path)}});
        return {{"ok", true}, {"workspaces", entries}};
    }
    if (type != "workspace.register" && type != "workspace.unregister")
        return fail("unknown_operation", "Unknown workspace operation.");
    const auto supplied = payload.value("path").toString();
    if (!QDir::isAbsolutePath(supplied))
        return fail("invalid_workspace", "Use an absolute workspace path.");
    auto path = QFileInfo(supplied).canonicalFilePath();
    if (type == "workspace.unregister") {
        // A deleted checkout can still be retired by its exact registered path.
        if (path.isEmpty())
            path = QDir::cleanPath(supplied);
        for (const auto& session : journal_->sessions())
            if (session.worktreePath == path)
                return fail("workspace_in_use",
                            "Delete this workspace's conversations before unregistering it.");
        if (!journal_->forgetWorktree(path))
            return fail("storage_error", "Unable to unregister the workspace.");
    } else {
        if (path.isEmpty() || !QFileInfo(path).isDir() ||
            !QFileInfo::exists(QDir(path).filePath(".git")))
            return fail("invalid_workspace", "Register the root of an existing Git checkout.");
        QProcess git;
        git.start(QStringLiteral("git"), {QStringLiteral("-C"), path, QStringLiteral("rev-parse"),
                                          QStringLiteral("--show-toplevel")});
        const bool checkout = git.waitForFinished(3000) &&
                              git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0 &&
                              QFileInfo(QString::fromUtf8(git.readAllStandardOutput()).trimmed())
                                      .canonicalFilePath() == path;
        if (!checkout) {
            if (git.state() != QProcess::NotRunning) {
                git.kill();
                git.waitForFinished(1000);
            }
            return fail("invalid_workspace", "Register the root of an existing Git checkout.");
        }
        if (!journal_->rememberWorktree(path))
            return fail("storage_error", "Unable to register the workspace.");
    }
    return {{"ok", true}, {"path", path}};
}
} // namespace ketplus::daemon
