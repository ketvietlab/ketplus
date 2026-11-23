#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace ketplus::daemon {

/// The checkouts of a repository, read from the files Git keeps for them instead of from a
/// `git worktree list` process, so the daemon thread can ask on every provider tool call.
///
/// A repository is keyed by its Git common directory and read again only when a stat shows
/// that its worktree list may have changed: the modification time or existence of
/// `<common>/worktrees` differs (adding or removing a linked worktree changes it), a listed
/// checkout is gone, or an entry left out because its checkout or `gitdir` file was missing
/// now has one. Not thread-safe; the session host uses it on its own thread.
class WorktreePathCache final {
  public:
    /// The canonical root of every checkout `git worktree list` reports as available for the
    /// repository that contains `directory`: the main checkout unless the repository is bare,
    /// then each linked checkout that still exists. Empty when `directory` is not in a Git
    /// checkout.
    [[nodiscard]] QStringList checkouts(const QString& directory);

  private:
    struct Entry final {
        // The administrative directory's `gitdir` file, and the checkout it names.
        QString gitdirFile;
        qint64 gitdirSize{-1};
        QString checkout;
        bool listed{false};
    };
    struct Repository final {
        bool administrationExists{false};
        QDateTime administrationModified;
        QList<Entry> entries;
        QStringList checkouts;
    };
    struct Location final {
        QString checkout;
        QString common;
    };

    [[nodiscard]] static Repository read(const QString& common);
    [[nodiscard]] static bool current(const Repository& repository, const QString& common);

    QHash<QString, Location> locations_;
    QHash<QString, Repository> repositories_;
};

} // namespace ketplus::daemon
