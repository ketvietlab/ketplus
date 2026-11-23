#pragma once

// ADR 0114 §10: what the team page's removal of a done task's worktree asks of the disk and git.
// Free functions, so the session runtime runs them off its event loop and tests run them on a
// real repository.
#include <QString>
#include <QtGlobal>

#include <functional>

class QObject;
class QThreadPool;

namespace ketplus::daemon::team_git {

// The bytes the folder takes on disk, without following links out of it.
[[nodiscard]] qint64 sizeOnDisk(const QString& folder);
// Refuses a main checkout, a worktree with uncommitted or untracked changes and one with
// commits on no remote branch; otherwise runs `git worktree remove` without force and then
// `git worktree prune` in `repository`. The error in words, or empty when it was removed.
[[nodiscard]] QString removeWorktree(const QString& repository, const QString& worktree);
// T-23: removeWorktree on a thread of `pool`, so deleting several gigabytes holds no event
// loop; `done` is called with its answer on the thread `receiver` lives on, and not at all
// once `receiver` is gone.
void removeInBackground(QThreadPool& pool, QObject* receiver, const QString& repository,
                        const QString& worktree, std::function<void(const QString&)> done);

} // namespace ketplus::daemon::team_git
