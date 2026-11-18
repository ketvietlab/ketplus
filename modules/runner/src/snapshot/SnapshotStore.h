#pragma once

#include "snapshot/RepositoryLayout.h"
#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"

#include <QDateTime>
#include <QMap>
#include <QString>

// What a snapshot store keeps, and the source HEAD of a worktree. Refs live only in the store,
// under storeRefPrefix; pruning removes the store's own unreferenced loose objects and never
// touches the repository the store reads from.
namespace ketplus::runner::snapshot {

inline constexpr auto storeRefPrefix = "refs/ketplus/";

// A ref name below storeRefPrefix: segments of [A-Za-z0-9_-] (a dot only inside a segment),
// separated by '/', at most 200 characters.
[[nodiscard]] bool isStoreRefName(const QString& name);

// Points storeRefPrefix + `name` at `commit`, which must already be in the store.
[[nodiscard]] Result<bool> keepRef(const QString& storePath, const QString& name,
                                   const QString& commit, const GitTool& tool = {});
// Removes the ref; true also when it did not exist.
[[nodiscard]] Result<bool> dropRef(const QString& storePath, const QString& name,
                                   const GitTool& tool = {});
// Every ref below storeRefPrefix: name (without the prefix) -> commit.
[[nodiscard]] Result<QMap<QString, QString>> listRefs(const QString& storePath,
                                                      const GitTool& tool = {});
// Deletes the store's loose objects that no ref reaches and that were last written before
// `expireBefore`. No gc or repack runs.
[[nodiscard]] Result<bool> pruneStore(const QString& storePath, const QDateTime& expireBefore,
                                      const GitTool& tool = {});

struct SourceHead {
    QString commit;          // lowercase hex of the layout's object format; empty when none
    bool unborn = false;     // HEAD names a branch without a commit
    bool unreadable = false; // HEAD could not be read (reftable, symlink, malformed)
};

// Reads HEAD of the worktree from files inside its git directories: a detached id, or a branch
// resolved through loose refs and packed-refs. Git never runs on the repository.
[[nodiscard]] SourceHead readSourceHead(const RepositoryLayout& layout);

} // namespace ketplus::runner::snapshot
