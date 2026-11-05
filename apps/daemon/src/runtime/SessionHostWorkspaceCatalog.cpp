#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
namespace ketplus::daemon {
QJsonObject SessionHost::workspaceCatalog(const QString& type, const QJsonObject& payload) {
    const auto fail = [](const char* code, const char* message) {
        return QJsonObject{{"ok", false}, {"error_code", code}, {"error_message", message}};
    };
    if (type == "workspace.list") {
        QJsonArray entries;
        for (const auto& path : journal_->worktreePaths())
            entries.append(QJsonObject{{"path", path}, {"available", QFileInfo(path).isDir()}});
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
