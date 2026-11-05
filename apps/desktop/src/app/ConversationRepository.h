#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ketplus {
// Exact execution roots have already been validated by the daemon. Resolve their
// main checkout without spawning Git on the GUI thread.
inline QString conversationRepositoryRoot(const QString& path) {
    if (path.isEmpty())
        return {};
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    const auto root = QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
    QFile pointer(QDir(root).filePath(".git"));
    if (!QFileInfo(pointer.fileName()).isFile() || !pointer.open(QIODevice::ReadOnly))
        return root;
    const auto line = QString::fromUtf8(pointer.readLine(4096)).trimmed();
    if (!line.startsWith("gitdir:"))
        return root;
    auto directory = line.mid(7).trimmed();
    if (QDir::isRelativePath(directory))
        directory = QDir(root).absoluteFilePath(directory);
    QDir git(QDir::cleanPath(directory));
    if (!git.cdUp() || git.dirName() != "worktrees" || !git.cdUp() || git.dirName() != ".git")
        return root;
    const QFileInfo main(QFileInfo(git.absolutePath()).absolutePath());
    return QDir::cleanPath(main.canonicalFilePath().isEmpty() ? main.absoluteFilePath()
                                                              : main.canonicalFilePath());
}
} // namespace ketplus
