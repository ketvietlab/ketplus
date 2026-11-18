#include "runner/sync/SnapshotPreparation.h"

#include "snapshot/RepositoryLayout.h"
#include "snapshot/SnapshotStore.h"
#include "snapshot/WorktreeSnapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace ketplus::daemon::runner::sync {

namespace snapshot = ketplus::runner::snapshot;

namespace {

QString codeOf(const snapshot::ErrorCode code) {
    switch (code) {
    case snapshot::ErrorCode::GitUnavailable:
    case snapshot::ErrorCode::GitTooOld:
        return prepare::gitUnsupported;
    case snapshot::ErrorCode::NotAWorktree:
        return prepare::notAWorktree;
    case snapshot::ErrorCode::UnsupportedRepository:
        return prepare::repositoryUnsupported;
    default:
        return prepare::snapshotBuildFailed;
    }
}

PreparedOutcome failed(const QString& code) { return {std::nullopt, code}; }

} // namespace

std::optional<WorktreeOf> worktreeOf(const QString& cwd) {
    const QString canonical = QFileInfo(cwd).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir())
        return std::nullopt;
    if (canonical.split(QLatin1Char('/')).contains(QStringLiteral(".git")))
        return std::nullopt;
    QDir directory(canonical);
    for (;;) {
        const QFileInfo marker(directory.filePath(QStringLiteral(".git")));
        if (marker.exists() || marker.isSymLink()) {
            const QString root = directory.absolutePath();
            const QString relative = directory.relativeFilePath(canonical);
            return WorktreeOf{root, relative == QStringLiteral(".") ? QString{} : relative};
        }
        if (directory.isRoot() || !directory.cdUp())
            return std::nullopt;
    }
}

QString storePathFor(const QString& storesRoot, const QString& repositoryId) {
    const QByteArray digest =
        QCryptographicHash::hash(repositoryId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(storesRoot).filePath(QString::fromLatin1(digest.left(32)) + QStringLiteral(".git"));
}

QString checkGit(const snapshot::GitTool& tool) {
    return snapshot::SealedGit(tool).checkVersion().ok() ? QString{} : prepare::gitUnsupported;
}

PreparedOutcome prepareSnapshot(const PrepareRequest& request, const snapshot::GitTool& tool) {
    if (const QString code = checkGit(tool); !code.isEmpty())
        return failed(code);
    const auto worktree = worktreeOf(request.cwd);
    if (!worktree)
        return failed(prepare::notAWorktree);
    const auto layout = snapshot::readRepositoryLayout(worktree->root);
    if (!layout)
        return failed(codeOf(layout.error().code));

    Prepared prepared;
    prepared.worktreeRoot = worktree->root;
    prepared.cwd = worktree->relative;
    prepared.worktreeId = worktree->root;
    prepared.repositoryId = QFileInfo(layout.value().commonDir).canonicalFilePath();
    if (prepared.repositoryId.isEmpty())
        return failed(prepare::notAWorktree);
    prepared.objectFormat = layout.value().objectFormat;
    prepared.storePath = storePathFor(request.storesRoot, prepared.repositoryId);

    snapshot::SnapshotRequest snapshotRequest;
    snapshotRequest.worktreePath = worktree->root;
    snapshotRequest.storePath = prepared.storePath;
    snapshotRequest.userGitSources = request.userGitSources;
    const auto made = snapshot::createSnapshot(snapshotRequest, tool);
    if (!made)
        return failed(codeOf(made.error().code));
    if (!snapshot::keepRef(prepared.storePath, request.inFlightRef, made.value().commit, tool))
        return failed(prepare::snapshotBuildFailed);

    prepared.snapshotId = made.value().commit;
    const snapshot::SourceHead head = snapshot::readSourceHead(layout.value());
    prepared.sourceHead = head.commit;
    prepared.sourceHeadUnreadable = head.unreadable;
    prepared.submodulePaths = made.value().submodulePaths;
    prepared.lfsPaths = made.value().lfsPaths;
    prepared.filteredPaths = made.value().filteredPaths;
    if (!prepared.submodulePaths.isEmpty())
        prepared.warnings.append(QStringLiteral("submodules_not_synced"));
    if (!prepared.lfsPaths.isEmpty())
        prepared.warnings.append(QStringLiteral("lfs_not_synced"));
    if (!prepared.filteredPaths.isEmpty())
        prepared.warnings.append(QStringLiteral("filtered_not_synced"));
    return {prepared, {}};
}

} // namespace ketplus::daemon::runner::sync
