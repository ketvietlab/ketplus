#include "runtime/WorktreePathCache.h"

#include "cloud/WorktreeRepositories.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ketplus::daemon {
namespace {

QString administration(const QString& common) {
    return QDir(common).filePath(QStringLiteral("worktrees"));
}

// The nearest directory, from `directory` up, that holds a `.git` directory or file.
QString checkoutContaining(const QString& directory) {
    const QString canonical = QFileInfo(directory).canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    QDir folder(canonical);
    do {
        if (QFileInfo::exists(folder.filePath(QStringLiteral(".git"))))
            return folder.absolutePath();
    } while (folder.cdUp());
    return {};
}

} // namespace

QStringList WorktreePathCache::checkouts(const QString& directory) {
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        return {};
    auto location = locations_.constFind(directory);
    if (location == locations_.cend() ||
        !QFileInfo::exists(QDir(location->checkout).filePath(QStringLiteral(".git"))) ||
        !QFileInfo(location->common).isDir()) {
        locations_.remove(directory);
        const QString checkout = checkoutContaining(directory);
        if (checkout.isEmpty())
            return {};
        const QString common = worktreeCommonDirectory(checkout);
        if (common.isEmpty())
            return {};
        location = locations_.insert(directory, {checkout, common});
    }
    const QString common = location->common;
    auto repository = repositories_.find(common);
    if (repository == repositories_.end() || !current(*repository, common))
        repository = repositories_.insert(common, read(common));
    return repository->checkouts;
}

WorktreePathCache::Repository WorktreePathCache::read(const QString& common) {
    Repository repository;
    const QFileInfo folder(administration(common));
    repository.administrationExists = folder.isDir();
    repository.administrationModified = folder.lastModified();
    // `git worktree list` names the parent of a `.git` common directory first; a bare
    // repository has no main checkout to work in.
    if (QFileInfo(common).fileName() == QStringLiteral(".git")) {
        const QString main = QFileInfo(QFileInfo(common).absolutePath()).canonicalFilePath();
        if (!main.isEmpty())
            repository.checkouts.append(main);
    }
    if (!repository.administrationExists)
        return repository;
    const QDir linked(folder.absoluteFilePath());
    for (const QString& name : linked.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        Entry entry;
        const QString own = linked.filePath(name);
        entry.gitdirFile = QDir(own).filePath(QStringLiteral("gitdir"));
        QFile gitdir(entry.gitdirFile);
        entry.gitdirSize = gitdir.exists() ? gitdir.size() : -1;
        if (entry.gitdirSize <= 4096 && gitdir.open(QIODevice::ReadOnly)) {
            // The path of the checkout's `.git` file, relative to this directory when Git
            // writes relative worktree paths.
            const QString value = QString::fromUtf8(gitdir.readLine()).trimmed();
            if (!value.isEmpty())
                entry.checkout = QDir::cleanPath(QDir(own).absoluteFilePath(value));
        }
        if (entry.checkout.endsWith(QStringLiteral("/.git")))
            entry.checkout.chop(5);
        // Git reports a checkout whose `.git` file is gone as prunable; the list leaves it out.
        const QString canonical = QFileInfo(entry.checkout).canonicalFilePath();
        entry.listed = !entry.checkout.isEmpty() && QFileInfo(entry.checkout).isDir() &&
                       QFileInfo::exists(QDir(entry.checkout).filePath(QStringLiteral(".git"))) &&
                       !canonical.isEmpty();
        if (entry.listed) {
            entry.checkout = canonical;
            if (!repository.checkouts.contains(canonical))
                repository.checkouts.append(canonical);
        }
        repository.entries.append(entry);
    }
    return repository;
}

// Stats only: no file is read and no process runs while the list stays the same.
bool WorktreePathCache::current(const Repository& repository, const QString& common) {
    const QFileInfo folder(administration(common));
    if (folder.isDir() != repository.administrationExists ||
        (repository.administrationExists &&
         folder.lastModified() != repository.administrationModified))
        return false;
    for (const Entry& entry : repository.entries) {
        if (entry.checkout.isEmpty()) {
            // An entry Git was still writing when it was read.
            const QFileInfo gitdir(entry.gitdirFile);
            if ((gitdir.exists() ? gitdir.size() : -1) != entry.gitdirSize)
                return false;
            continue;
        }
        const bool present =
            QFileInfo::exists(QDir(entry.checkout).filePath(QStringLiteral(".git")));
        if (present != entry.listed)
            return false;
    }
    return true;
}

} // namespace ketplus::daemon
