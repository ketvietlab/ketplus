#pragma once

#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

// Desktop side of a runner job's source: freeze a worktree's current state, including
// uncommitted and untracked changes, as a commit in a KetPlus-owned store, then pack what a
// runner needs beyond the snapshot it already holds. The user's repository is only read.
namespace ketplus::runner::snapshot {

struct SnapshotRequest {
    QString worktreePath; // absolute worktree root; a linked worktree is fine
    // A KetPlus-owned bare repository outside the user's repository, created on first use.
    // It reads the repository's objects without ever writing to them, and is bound to one
    // repository (all of that repository's worktrees may share it).
    QString storePath;
};

struct Snapshot {
    QString commit; // parentless commit in the store, fixed identity and date
    QString tree;
    // Gitlinks in the snapshot. Only the recorded commit id travels, never the submodule's
    // content; the caller warns that the runner sees an empty directory there.
    QStringList submodulePaths;
    // Paths with the `filter=lfs` attribute. A tracked one keeps its index blob (normally the
    // LFS pointer), never the large worktree content; an untracked one is left out entirely.
    QStringList lfsPaths;
};

// Captures the worktree as it is now: every tracked path with its current content (staged
// or not), deletions, renames, the executable bit and symlinks, plus untracked files that are
// not ignored by .gitignore or the repository's info/exclude. Global excludes are not read.
[[nodiscard]] Result<Snapshot> createSnapshot(const SnapshotRequest& request,
                                              const GitTool& tool = {});

struct PackRequest {
    QString storePath;
    QString commit;     // snapshot to send
    QString baseCommit; // snapshot the runner already holds; empty sends everything
    QString outputPath; // pack file to write (replaced if present)
};

struct Pack {
    QString path;
    qint64 size = 0;
    QByteArray sha256; // lowercase hex
    QString commit;
    QString baseCommit;
};

// Writes a pack of the objects reachable from `commit` but not from `baseCommit`. Fails with
// BaseMissing when the base is no longer in the store; the caller then sends a full pack.
[[nodiscard]] Result<Pack> writePack(const PackRequest& request, const GitTool& tool = {});

// Lowercase hex SHA-256 of a file, or empty when it cannot be read.
[[nodiscard]] QByteArray fileSha256(const QString& path);

} // namespace ketplus::runner::snapshot
