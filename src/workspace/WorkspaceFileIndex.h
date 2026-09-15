#pragma once

#include <QString>
#include <QStringList>

namespace ketplus {

struct WorkspaceFileList final {
    QStringList relativePaths;
    bool truncated{false};
};

inline constexpr int defaultWorkspaceFileLimit = 50000;

// Lists files under `rootPath` relative to it, sorted, skipping VCS metadata,
// dependency and build folders. Safe to call from a worker thread.
[[nodiscard]] WorkspaceFileList collectWorkspaceFiles(const QString& rootPath,
                                                      int maximumFiles = defaultWorkspaceFileLimit);

} // namespace ketplus
