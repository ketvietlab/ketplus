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

// The value of `core.<key>` in the repository's own config file, without includes.
QString coreSetting(const QString& common, const QString& key) {
    QFile config(QDir(common).filePath(QStringLiteral("config")));
    if (config.size() > 1024 * 1024 || !config.open(QIODevice::ReadOnly))
        return {};
    bool inCore = false;
    while (!config.atEnd()) {
        const QString line = QString::fromUtf8(config.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inCore = line.compare(QStringLiteral("[core]"), Qt::CaseInsensitive) == 0;
            continue;
        }
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (!inCore || equals < 0 || line.left(equals).trimmed().compare(key, Qt::CaseInsensitive))
            continue;
        QString value = line.mid(equals + 1).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) &&
            value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        return value;
    }
    return {};
}

// The first entry of `git worktree list`. A common directory called `.git` is listed as the
// checkout beside it. Git lists any other one (`--separate-git-dir`, a submodule's directory
// under `.git/modules`) as the Git directory itself, whatever `core.worktree` says, and a bare
// repository is listed with a `bare` flag that leaves it out of the usable checkouts. This
// keeps what the list of `git worktree list` gave before, quirk included.
QString mainCheckout(const QString& common) {
    if (QFileInfo(common).fileName() == QStringLiteral(".git"))
        return QFileInfo(QFileInfo(common).absolutePath()).canonicalFilePath();
    if (coreSetting(common, QStringLiteral("bare"))
            .compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0)
        return {};
    return QFileInfo(common).canonicalFilePath();
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
    if (const QString main = mainCheckout(common); !main.isEmpty())
        repository.checkouts.append(main);
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
//
// Known limit: the directory's modification time is the only sign that a checkout was added or
// removed. Two changes within one tick of the file system's clock, with a read in between,
// leave the later one unseen until the next change (or until a listed checkout's `.git`
// disappears); the list is then stale for tool calls in that checkout only.
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
