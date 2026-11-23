#pragma once

#include <QString>

namespace ketplus::git {

// A linked checkout and its Git registration must still name each other.
[[nodiscard]] QString linkedWorktreeIdentity(const QString& worktree);

// Run off the UI or daemon event loop. An empty result means the directory was removed.
[[nodiscard]] QString removeLinkedWorktree(const QString& repository, const QString& worktree,
                                           const QString& expectedIdentity = {});

} // namespace ketplus::git
