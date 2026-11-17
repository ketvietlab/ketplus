#pragma once

#include "snapshot/SnapshotResult.h"

#include <QString>

// Where a worktree keeps its git data, found by reading files rather than by running git
// against the user's repository: running git there would read that repository's own config
// (hooks, filter drivers, fsmonitor), which the snapshot must never execute.
namespace ketplus::runner::snapshot {

struct RepositoryLayout {
    QString workTree;     // canonical worktree root
    QString gitDir;       // per-worktree directory: <repo>/.git or <repo>/.git/worktrees/<name>
    QString commonDir;    // shared directory holding objects and refs
    QString objectsDir;   // <commonDir>/objects
    QString indexFile;    // <gitDir>/index; may not exist in a new repository
    QString objectFormat; // "sha1" or "sha256"
};

// Reads `<worktree>/.git`, either a directory or a `gitdir:` file (a linked worktree or a
// submodule checkout). Fails with NotAWorktree for a path that is not a worktree root.
[[nodiscard]] Result<RepositoryLayout> readRepositoryLayout(const QString& worktreePath);

} // namespace ketplus::runner::snapshot
