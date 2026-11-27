#pragma once

#include "snapshot/SealedGit.h"
#include "snapshot/UserGitRules.h"

#include <QString>
#include <QStringList>

#include <optional>

// Freezes the worktree of a job's cwd as a snapshot in the KetPlus store of its repository and
// returns what the job spec needs. Runs Git; call it off the event loop.
namespace ketplus::daemon::runner::sync {

namespace prepare {
inline const QString gitUnsupported = QStringLiteral("git_unsupported");
inline const QString notAWorktree = QStringLiteral("not_a_worktree");
inline const QString repositoryUnsupported = QStringLiteral("repository_unsupported");
inline const QString snapshotBuildFailed = QStringLiteral("snapshot_build_failed");
} // namespace prepare

// The worktree root holding `cwd` (the nearest directory with a `.git` entry) and `cwd`
// relative to it in POSIX form; nullopt when cwd is outside every worktree or inside `.git`.
struct WorktreeOf {
    QString root;     // canonical
    QString relative; // empty for the root itself
};
[[nodiscard]] std::optional<WorktreeOf> worktreeOf(const QString& cwd);

struct Prepared {
    QString worktreeRoot;
    QString cwd;          // relative to the root
    QString repositoryId; // canonical common git directory; enters only the key HMAC
    QString worktreeId;   // canonical worktree root; enters only the key HMAC
    QString storePath;
    QString snapshotId;
    QString sourceHead; // empty without a commit or when HEAD could not be read
    bool sourceHeadUnreadable = false;
    QString objectFormat;
    QStringList warnings; // wire codes, no duplicates
    // Paths behind each warning, for the user only: never sent and never logged.
    QStringList submodulePaths;
    QStringList lfsPaths;
    QStringList filteredPaths;
};

struct PreparedOutcome {
    std::optional<Prepared> prepared;
    QString code; // one of prepare::* when there is nothing prepared
};

struct PrepareRequest {
    QString cwd;
    QString storesRoot;  // directory holding one store per repository
    QString inFlightRef; // store ref name held for the job until it ends
    std::optional<ketplus::runner::snapshot::UserGitSources> userGitSources;
};

// Git missing or older than the snapshot library needs: git_unsupported; empty when usable.
[[nodiscard]] QString checkGit(const ketplus::runner::snapshot::GitTool& tool = {});

[[nodiscard]] PreparedOutcome prepareSnapshot(const PrepareRequest& request,
                                              const ketplus::runner::snapshot::GitTool& tool = {});

// The store directory of a repository below `storesRoot`, named by a digest of its id.
[[nodiscard]] QString storePathFor(const QString& storesRoot, const QString& repositoryId);

} // namespace ketplus::daemon::runner::sync
