#pragma once

#include <QString>
#include <QStringList>

namespace ketplus::daemon {

/// Every checkout of a repository that still exists: the main one first, then each linked
/// worktree Git lists under `.git/worktrees`, in name order.
QStringList repositoryCheckouts(const QString& repository);

/// The branch a checkout has out (`feat/x`), or empty when its HEAD is detached.
QString checkoutBranch(const QString& worktree);

struct RepositoryBranches {
    /// As Git writes them: `develop`, and `origin/develop` for a remote-tracking branch.
    QStringList local;
    QStringList remote;
    bool ok{false};
};

/// A repository's local and remote-tracking branches, read with `git for-each-ref` and no
/// fetch. Blocks for up to 10 s and reads at most 1 MiB, so it runs off the main thread.
RepositoryBranches readRepositoryBranches(const QString& repository);

/// Whether Git would take `name` for a new branch: the rules of
/// `git check-ref-format --branch`, checked without running Git.
bool validNewBranchName(const QString& name);

/// Where the desktop puts a new worktree: beside the main checkout, named
/// <repository>-<branch> with slashes turned into dashes.
QString newWorktreePath(const QString& repository, const QString& branch);

struct AddedWorktree {
    QString path;
    /// Empty on success; otherwise `branch_exists`, `worktree_exists`, `invalid_branch_name`,
    /// `base_branch_not_found` or `worktree_create_failed`.
    QString errorCode;
    QString message;
};

/// Cuts `branch` from `base` (a name `readRepositoryBranches` lists) into a new worktree at
/// `newWorktreePath`, as the desktop's New conversation dialog does. Blocks for up to 30 s,
/// so it runs off the main thread.
AddedWorktree addWorktree(const QString& repository, const QString& branch, const QString& base);

} // namespace ketplus::daemon
