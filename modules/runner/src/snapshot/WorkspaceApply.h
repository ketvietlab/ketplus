#pragma once

#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"

#include <QByteArray>
#include <QString>

// Runner side of a job's source: take a pack from the desktop, which is untrusted input, into
// a runner-owned bare mirror, then bring a workspace to exactly the snapshot's tree while
// keeping everything an incremental build relies on.
namespace ketplus::runner::snapshot {

struct ApplyLimits {
    qint64 maxPackBytes = qint64(2) * 1024 * 1024 * 1024;
    // Sum of blob sizes in the snapshot tree; a small pack can expand into far more.
    qint64 maxCheckoutBytes = qint64(32) * 1024 * 1024 * 1024;
    qint64 maxEntries = 2'000'000;
};

struct ApplyRequest {
    QString mirrorPath;    // runner-owned bare repository, created on first use
    QString workspacePath; // absolute; created if missing
    QString packPath;
    QByteArray sha256; // expected lowercase hex SHA-256 of the pack
    QString commit;    // the snapshot the pack carries
    ApplyLimits limits;
    // Delete every path the workspace's index does not track before the checkout (see
    // cleanWorkspace in WorkspaceStore.h); only once every check has passed.
    bool clean = false;
};

struct Applied {
    QString commit;
    QString previousCommit; // the snapshot this workspace held before, if any
};

// Rejects the pack before touching the workspace when it is too large, its digest differs,
// git's strict checks fail, the commit or any object it reaches is missing, a path could
// leave the workspace (`..`, an absolute path, a `.git` component) or the tree exceeds the
// limits; the workspace is then exactly as before.
//
// Once accepted, the workspace is checked out from a per-workspace index kept in the mirror:
// only files whose content or mode differs are rewritten (unchanged files keep their mtime),
// files the previous snapshot tracked and this one drops are deleted, and untracked or
// ignored files (a build directory) stay unless the snapshot itself claims their path. Git
// never writes through a symlink, so a link in the snapshot or the workspace cannot direct a
// write outside the workspace.
[[nodiscard]] Result<Applied> applySnapshot(const ApplyRequest& request, const GitTool& tool = {});

// The same as applySnapshot for a snapshot the mirror already holds, without a pack: every
// check from the commit on, then the same checkout. SnapshotIncomplete when the mirror lacks
// the commit or an object it reaches.
struct MirrorApplyRequest {
    QString mirrorPath;
    QString workspacePath;
    QString commit;
    ApplyLimits limits;
    bool clean = false;
};
[[nodiscard]] Result<Applied> applyFromMirror(const MirrorApplyRequest& request,
                                              const GitTool& tool = {});

// Whether a tree path from an untrusted snapshot is safe to create under a workspace.
[[nodiscard]] bool isSafeSnapshotPath(const QString& path);

} // namespace ketplus::runner::snapshot
