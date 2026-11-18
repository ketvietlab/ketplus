#pragma once

#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"
#include "snapshot/UserGitRules.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

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
    // Where the user's git configuration is looked up; unset uses the caller's environment.
    std::optional<UserGitSources> userGitSources = std::nullopt;
};

struct Snapshot {
    // Parentless commit in the store, fixed identity and date. The store holds the commit and
    // every tree; a blob the repository already has is read from there, never copied.
    QString commit;
    QString tree;
    // Gitlinks in the snapshot. Only the recorded commit id travels, never the submodule's
    // content; the caller warns that the runner sees an empty directory there.
    QStringList submodulePaths;
    // Paths with a `filter` attribute from any source but the system one: .gitattributes in
    // the tree, the repository's info/attributes, or core.attributesFile (global or repository
    // configuration; by default $XDG_CONFIG_HOME/git/attributes). No filter runs and no worktree
    // content of these paths enters the snapshot: a tracked one keeps the blob its index already
    // holds (the LFS pointer, or the encrypted form a filter such as git-crypt stored), an
    // untracked one is left out.
    QStringList lfsPaths;      // filter=lfs
    QStringList filteredPaths; // any other filter
};

// Captures the worktree as it is now: every tracked path with its current content (staged
// or not), deletions, renames, the executable bit and symlinks, plus untracked files that are
// not ignored by .gitignore, the repository's info/exclude or the user's core.excludesFile.
// Fails with SnapshotIncomplete rather than return a snapshot with an unreadable object. The
// only git command run on the user's repository is SealedGit::queryUserConfig; the others
// run on the store and at most read the repository's objects.
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
