#pragma once

#include "snapshot/RepositoryLayout.h"
#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"

#include <QByteArray>
#include <QString>

// The user's own ignore and attribute files (core.excludesFile and core.attributesFile, or
// their defaults under $XDG_CONFIG_HOME/git), so a snapshot hides what the user's git hides
// and treats as filtered what the user's git filters. The values are resolved by git itself
// through SealedGit::queryUserConfig; the files they name are read as bounded data.
namespace ketplus::runner::snapshot {

// Bytes accepted from either file; a larger one fails the snapshot rather than being dropped.
inline constexpr qint64 maxUserRulesFileBytes = 1024 * 1024;

// Reads an ignore or attributes file as data. Missing or not a regular file (a device, FIFO
// or directory): empty. Over maxUserRulesFileBytes: UnsupportedRepository, since dropping
// the file would put ignored or filtered paths into the snapshot.
[[nodiscard]] Result<QByteArray> readRulesFile(const QString& path);

// Where the user's git would look, as seen by that git: empty means unset.
struct UserGitSources {
    QString home;
    QString xdgConfigHome;
    QString globalConfig; // GIT_CONFIG_GLOBAL
};

// The caller's own environment: QDir::homePath(), XDG_CONFIG_HOME and GIT_CONFIG_GLOBAL.
[[nodiscard]] UserGitSources userGitSourcesFromEnvironment();

struct UserGitRules {
    QByteArray excludes;
    QByteArray attributes;
};

// Each file the configuration names (or its default) is read with readRulesFile.
[[nodiscard]] Result<UserGitRules> readUserGitRules(const SealedGit& git,
                                                    const RepositoryLayout& layout,
                                                    const UserGitSources& sources);

} // namespace ketplus::runner::snapshot
