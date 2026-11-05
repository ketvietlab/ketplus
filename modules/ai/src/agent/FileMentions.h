#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

namespace ketplus::file_mentions {

// A composer mention: `@src/main.cpp`, `@docs/` or `@"path with spaces"`.
struct Mention {
    qsizetype start{0};
    qsizetype end{0};
    QString path;
};

enum class Resolution { Inside, Missing, Outside };

struct ResolvedMention {
    Resolution resolution{Resolution::Missing};
    QString relativePath; // relative to the worktree, folders end with '/'
    bool directory{false};
};

// Workspace entries relative to root; folders end with '/'. Tracked and untracked files that
// are not ignored when root is a Git worktree, otherwise a filtered directory walk.
[[nodiscard]] QStringList listEntries(const QString& root, qsizetype maximumEntries = 50000);

[[nodiscard]] QStringList rankEntries(const QStringList& entries, const QString& query,
                                      qsizetype maximumResults = 8);
// Same, with `foldedEntries[i] == entries[i].toCaseFolded()` computed once per listing, so a
// keystroke does not fold every path of a large worktree again.
[[nodiscard]] QStringList rankEntries(const QStringList& entries, const QStringList& foldedEntries,
                                      const QString& query, qsizetype maximumResults = 8);

[[nodiscard]] QVector<Mention> parseMentions(const QString& prompt);

// Resolves a mention against the conversation worktree. Absolute, home-relative and `..`
// paths, and symlinks leading out of the worktree, resolve to Outside.
[[nodiscard]] ResolvedMention resolve(const QString& root, const QString& path);

// Prompt preamble naming the worktree paths the user mentioned; each item has `path` and
// `directory`.
[[nodiscard]] QString referencedPathInstructions(const QVariantList& paths);

[[nodiscard]] QString insertionText(const QString& relativePath);

} // namespace ketplus::file_mentions
