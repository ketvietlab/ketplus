#pragma once

#include "snapshot/RepositoryLayout.h"
#include "snapshot/SealedGit.h"

#include <QByteArray>
#include <QString>

// The ignore rules a snapshot applies on top of the worktree's .gitignore files, in the
// order git gives them precedence: core.excludesFile (or its default,
// $XDG_CONFIG_HOME/git/ignore), then the repository's info/exclude, whose later lines win.
// Configuration is only read as data; nothing it names is executed.
namespace ketplus::runner::snapshot {

// Bytes read from any one exclude file; a larger file is ignored rather than truncated.
inline constexpr qint64 maxExcludeFileBytes = 1024 * 1024;

struct ExcludeSources {
    QString home;          // the user's home directory
    QString xdgConfigHome; // $XDG_CONFIG_HOME, or empty for <home>/.config
    QString globalConfig;  // $GIT_CONFIG_GLOBAL, or empty for the default global files
};

// The caller's own environment: QDir::homePath(), XDG_CONFIG_HOME and GIT_CONFIG_GLOBAL.
[[nodiscard]] ExcludeSources excludeSourcesFromEnvironment();

// Content for the store's info/exclude.
[[nodiscard]] QByteArray collectExcludeRules(const SealedGit& git, const RepositoryLayout& layout,
                                             const ExcludeSources& sources);

} // namespace ketplus::runner::snapshot
