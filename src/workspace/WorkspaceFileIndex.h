#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

namespace ketplus {

struct WorkspaceFileList final {
    QStringList relativePaths;
    bool truncated{false};
};

inline constexpr int defaultWorkspaceFileLimit = 50000;
// What a workspace may grow to once the user accepts scanning a tree beyond the default.
inline constexpr int maximumWorkspaceFileLimit = 5000000;

// True for suffixes never worth opening as text: images, media, archives, fonts, documents
// and compiled artefacts. Checked before a file is read, which matters in trees that keep
// thousands of assets beside their source.
[[nodiscard]] bool hasBinaryFileSuffix(const QString& path);

// Lists files under `rootPath` relative to it, sorted, skipping VCS metadata,
// dependency and build folders. Safe to call from a worker thread; stops early and
// returns what it has when `cancelled` becomes true.
[[nodiscard]] WorkspaceFileList collectWorkspaceFiles(const QString& rootPath,
                                                      int maximumFiles = defaultWorkspaceFileLimit,
                                                      const std::atomic_bool* cancelled = nullptr);

} // namespace ketplus
