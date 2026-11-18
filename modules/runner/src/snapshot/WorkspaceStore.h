#pragma once

#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"

#include <QString>

#include <optional>

// What a runner mirror keeps for each workspace it checked out, and the questions a receiver
// asks the mirror before it takes a pack.
namespace ketplus::runner::snapshot {

// The index and the ref the mirror keeps for a workspace, named after the workspace's
// canonical path.
struct WorkspaceState {
    QString indexFile;
    QString ref;
};
[[nodiscard]] WorkspaceState workspaceState(const QString& mirrorPath,
                                            const QString& canonicalWorkspace);

// Whether `commit` is a commit object in the mirror. A missing mirror holds nothing.
[[nodiscard]] Result<bool> mirrorHasCommit(const QString& mirrorPath, const QString& commit,
                                           const GitTool& tool = {});

// Whether the mirror holds the whole snapshot: the commit and every object it reaches.
[[nodiscard]] Result<bool> mirrorHasSnapshot(const QString& mirrorPath, const QString& commit,
                                             const GitTool& tool = {});

// Deletes every path of the workspace that its index does not track (all of them without an
// index), including directories holding a `.git`, and every `.git` entry anywhere in the tree.
// Nothing is deleted through a symlink: a link is removed itself, never followed.
[[nodiscard]] std::optional<Error> cleanWorkspace(const SealedGit& git, const QString& mirrorPath,
                                                  const QString& canonicalWorkspace);

// Removes a workspace: its tree, then the index and ref the mirror keeps for it. A workspace
// that is already gone is fine.
[[nodiscard]] Result<bool> removeWorkspace(const QString& mirrorPath, const QString& workspacePath,
                                           const GitTool& tool = {});

} // namespace ketplus::runner::snapshot
