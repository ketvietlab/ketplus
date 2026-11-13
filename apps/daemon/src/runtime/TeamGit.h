#pragma once

// What a team asks git (ADR 0114): free functions over copies of a team and its tasks, so a
// thread other than the session runtime's can run them.
#include "teams/TeamCoordinator.h"

#include <QSet>

#include <optional>

namespace ketplus::daemon::team_git {

// A short git call in `directory`; nothing when git failed or did not answer in time.
[[nodiscard]] std::optional<QString> output(const QString& directory, const QStringList& arguments);
// The main worktree of the repository `worktreePath` is in (a bare repository is its own);
// empty when the path is no repository or git did not answer in time.
[[nodiscard]] QString repositoryOf(const QString& worktreePath);
// The commit the task's branch points at in the team's repository; empty when unknown.
[[nodiscard]] QString taskCommit(const teams::Team& team, const teams::Task& task);
// Whether `commit` is an ancestor of `of` (or the same) in `repository`; nothing when git could
// not tell.
[[nodiscard]] std::optional<bool> isAncestor(const QString& repository, const QString& commit,
                                             const QString& of);
// Whether the task's branch exists in the team's repository; nothing when git failed.
[[nodiscard]] std::optional<bool> branchExists(const teams::Team& team, const teams::Task& task);
// T-38: the commit the task's branch points at when it is the task's own: made on the branch
// after its base and not yet in the integration branch. Empty otherwise; nothing when git
// could not read the branch or its ancestry.
[[nodiscard]] std::optional<QString> ownHead(const teams::Team& team, const teams::Task& task);
// Tracked paths the task's branch changed against its base, and, while its worktree has the
// branch checked out, the tracked files changed there but not committed. Never untracked or
// ignored files.
[[nodiscard]] std::optional<QStringList> changedPaths(const teams::Team& team,
                                                      const teams::Task& task);
// Whether the integration branch contains the task's branch; nothing when git failed.
[[nodiscard]] std::optional<bool> isMerged(const teams::Team& team, const teams::Task& task);
// The task's branch against its base on the integration branch; nothing when git failed.
[[nodiscard]] std::optional<teams::TaskDiff> taskDiff(const teams::Team& team,
                                                      const teams::Task& task);
// T-90 (ADR 0123): whether a team or task may work in `path`: an absolute path to the main
// checkout of a Git repository whose origin has `integrationBranch` as a remote-tracking
// branch (nothing is fetched). The canonical path, or why not.
[[nodiscard]] teams::RepositoryCheck checkRepository(const QString& path,
                                                     const QString& integrationBranch);
// T-93 (ADR 0121 §9): whether `branch` is a local branch of `repository` or one of origin's
// remote-tracking branches. Nothing when git could not answer.
[[nodiscard]] std::optional<bool> branchTaken(const QString& repository, const QString& branch);
// The names of `repository`'s local branches and origin's remote-tracking branches, as
// `branchTaken` reads them, in one git call. Nothing when git could not answer.
[[nodiscard]] std::optional<QSet<QString>> branchNames(const QString& repository);
// Creates the task's branch and worktree beside the repository (ADR 0114, open point 1).
[[nodiscard]] teams::WorktreeAnswer createWorktree(const teams::Team& team,
                                                   const teams::Task& task);
// Whether the worktree has changes that are not committed; nothing when git failed.
[[nodiscard]] std::optional<bool> hasUncommittedWork(const QString& worktree);
// What a hand to review reads of the task.
[[nodiscard]] teams::HandoffFacts handoff(const teams::Team& team, const teams::Task& task);
// The answers to one check, by task id.
[[nodiscard]] QHash<QString, teams::TaskFacts>
facts(const teams::Team& team, const QList<teams::Task>& tasks, teams::TaskQuestion question);

} // namespace ketplus::daemon::team_git
