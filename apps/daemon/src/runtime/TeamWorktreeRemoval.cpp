// ADR 0114 §10: the size of a worktree and its removal, off the session runtime's event loop.
#include "runtime/TeamWorktreeRemoval.h"

#include "LinkedWorktreeRemoval.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QPointer>
#include <QThreadPool>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace ketplus::daemon::team_git {
// The bytes the folder takes on disk, without following links out of it.
qint64 sizeOnDisk(const QString& folder) {
    qint64 total = 0;
    QDirIterator files(folder, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
#ifdef Q_OS_UNIX
        struct stat status{};
        if (::lstat(QFile::encodeName(path).constData(), &status) == 0) {
            total += static_cast<qint64>(status.st_blocks) * 512;
            continue;
        }
#endif
        total += QFileInfo(path).size();
    }
    return total;
}

QString removeWorktree(const QString& repository, const QString& worktree) {
    return ketplus::git::removeLinkedWorktree(repository, worktree);
}

void removeInBackground(QThreadPool& pool, QObject* receiver, const QString& repository,
                        const QString& worktree, std::function<void(const QString&)> done) {
    pool.start([receiver = QPointer<QObject>(receiver), repository, worktree,
                done = std::move(done)]() mutable {
        QString error = removeWorktree(repository, worktree);
        if (!receiver)
            return;
        QMetaObject::invokeMethod(
            receiver.data(), [done = std::move(done), error = std::move(error)] { done(error); },
            Qt::QueuedConnection);
    });
}

} // namespace ketplus::daemon::team_git
