#include "cloud/WorktreeRepositories.h"

#include "storage/LocalJournal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace ketplus::daemon {

QString worktreeGitDirectory(const QString& worktree) {
    const QFileInfo dotGit(QDir(worktree).filePath(QStringLiteral(".git")));
    if (dotGit.isDir())
        return dotGit.absoluteFilePath();
    if (!dotGit.isFile())
        return {};
    QFile marker(dotGit.absoluteFilePath());
    if (!marker.open(QIODevice::ReadOnly))
        return {};
    const QString value = QString::fromUtf8(marker.readLine()).trimmed();
    if (!value.startsWith(QStringLiteral("gitdir: ")))
        return {};
    const QString candidate = value.mid(8).trimmed();
    return QFileInfo(candidate).isAbsolute() ? candidate
                                             : QDir(worktree).absoluteFilePath(candidate);
}

QString worktreeCommonDirectory(const QString& worktree) {
    const QString gitDirectory = worktreeGitDirectory(worktree);
    if (gitDirectory.isEmpty())
        return {};
    // A linked checkout's Git directory names the shared one in its commondir file.
    QString common = gitDirectory;
    QFile marker(QDir(gitDirectory).filePath(QStringLiteral("commondir")));
    if (marker.open(QIODevice::ReadOnly)) {
        const QString value = QString::fromUtf8(marker.readLine()).trimmed();
        if (!value.isEmpty())
            common = QFileInfo(value).isAbsolute() ? value : QDir(gitDirectory).filePath(value);
    }
    return QFileInfo(common).canonicalFilePath();
}

QString worktreeRepository(const QString& worktree) {
    const QString gitDirectory = worktreeGitDirectory(worktree);
    const qsizetype linked = gitDirectory.indexOf(QStringLiteral("/.git/worktrees/"));
    return linked > 0 ? gitDirectory.left(linked) : worktree;
}

namespace {

// A deleted worktree has no canonical path of its own; its parent usually still does, which
// matters where the parent is reached through a link, such as /tmp on macOS.
QString comparablePath(const QString& path) {
    const QFileInfo info(path);
    if (const QString canonical = info.canonicalFilePath(); !canonical.isEmpty())
        return canonical;
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return QDir::cleanPath(parent.isEmpty() ? info.absoluteFilePath()
                                            : QDir(parent).filePath(info.fileName()));
}

} // namespace

QHash<QString, QString> resolveWorktreeRepositories(LocalJournal& journal) {
    QHash<QString, QString> repositories = journal.worktreeRepositories();
    QSet<QString> known;
    QStringList unresolved;
    for (auto it = repositories.begin(); it != repositories.end(); ++it) {
        const QString worktree = comparablePath(it.key());
        if (QFileInfo(worktree).isDir()) {
            const QString repository = worktreeRepository(worktree);
            known.insert(repository);
            if (repository == worktree)
                continue;
            if (it.value() != repository && journal.rememberWorktreeRepository(it.key(), repository))
                it.value() = repository;
        } else if (it.value().isEmpty()) {
            unresolved.append(it.key());
        } else {
            known.insert(it.value());
        }
    }
    if (unresolved.isEmpty())
        return repositories;
    // Git lists each linked worktree under <repository>/.git/worktrees/<name>/gitdir,
    // which holds the path of the worktree's .git file, until the entry is pruned.
    QHash<QString, QString> registered;
    for (const QString& repository : std::as_const(known)) {
        const QDir administration(QDir(repository).filePath(QStringLiteral(".git/worktrees")));
        for (const QString& name : administration.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile gitdir(administration.filePath(name + QStringLiteral("/gitdir")));
            if (!gitdir.open(QIODevice::ReadOnly))
                continue;
            const QString dotGit = QString::fromUtf8(gitdir.readLine()).trimmed();
            if (dotGit.endsWith(QStringLiteral("/.git")))
                registered.insert(QDir::cleanPath(dotGit.chopped(5)), repository);
        }
    }
    for (const QString& worktree : std::as_const(unresolved)) {
        const QString path = comparablePath(worktree);
        QString repository = registered.value(path);
        // Once pruned, only the name is left. The desktop creates a worktree beside its
        // repository as <repository>-<branch>, so the longest such sibling checkout is taken.
        if (repository.isEmpty()) {
            const QFileInfo info(path);
            const QDir parent(info.absolutePath());
            QString name;
            for (const QString& sibling : parent.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (sibling.size() > name.size() &&
                    info.fileName().startsWith(sibling + QLatin1Char('-')) &&
                    QFileInfo(parent.filePath(sibling + QStringLiteral("/.git"))).isDir())
                    name = sibling;
            }
            if (!name.isEmpty())
                repository = parent.filePath(name);
        }
        if (!repository.isEmpty() && journal.rememberWorktreeRepository(worktree, repository))
            repositories.insert(worktree, repository);
    }
    return repositories;
}

} // namespace ketplus::daemon
