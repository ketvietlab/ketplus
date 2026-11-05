#pragma once

#include <QHash>
#include <QString>

namespace ketplus::daemon {

class LocalJournal;

/// The Git directory of a worktree: `.git` itself, or the one its `.git` file points at.
/// Empty when the worktree is gone or is not a Git checkout.
QString worktreeGitDirectory(const QString& worktree);

/// The main checkout a linked worktree belongs to, or the worktree itself.
QString worktreeRepository(const QString& worktree);

/// The repository of every catalog worktree, keyed by its catalog path. A linked worktree's
/// repository is recorded while it exists, so its conversations stay under that repository
/// after it is deleted. One deleted before it was recorded is found through the worktree list
/// Git keeps in each known repository until it is pruned, and after that by the
/// <repository>-<branch> name the desktop gives it beside the repository. Empty when none
/// of these apply.
QHash<QString, QString> resolveWorktreeRepositories(LocalJournal& journal);

} // namespace ketplus::daemon
