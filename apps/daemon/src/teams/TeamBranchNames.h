#pragma once

// T-93 (ADR 0121 §9): the branch and worktree a new task is given, `<type>/<slug>-t<n>` and
// `<repository>-<type>-<slug>-t<n>`, from the leader's branch_type/branch_slug or from the
// task's title. A task without a slug, as every task created before, keeps `team/<id>/t-<n>`.
#include "teams/Team.h"
#include "teams/TeamRules.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::daemon::teams::branch_names {

inline constexpr qsizetype maximumSlug = 40;

// feat, fix, chore, docs, refactor and test.
[[nodiscard]] const QStringList& types();

// The slug of a title: diacritics removed (đ is d), lower case, words of letters and digits
// joined by `-`, without stop words, numbers, task ids, a leading type word or bracketed
// asides, cut at a word boundary to maximumSlug characters. "task" when nothing is left.
[[nodiscard]] QString slugOf(const QString& title);

// The type a title reads as: its first word when that names a type (fix, docs, sửa, ...),
// else fix when a word names a defect (bug, crash, lỗi, ...), else feat.
[[nodiscard]] QString typeOf(const QString& title);

// A type and slug the leader names: a type of types() and a slug of lower-case letters and
// digits in words joined by single `-`, at most maximumSlug characters.
[[nodiscard]] std::optional<TeamError> validType(const QString& type);
[[nodiscard]] std::optional<TeamError> validSlug(const QString& slug);

// The number of a task id ("T-93" is 93); 0 for none.
[[nodiscard]] int taskNumber(const QString& taskId);

// The branch a task gets when its worktree is made: `<type>/<slug>-t<n>` for a task with a
// slug, `team/<team>/t-<n>` for one without.
[[nodiscard]] QString plannedBranch(qint64 teamId, const Task& task);
// The worktree folder's name beside its repository: `<repository>-<type>-<slug>-t<n>`, or
// `<repository>-team-<team>-t-<n>` for a task without a slug.
[[nodiscard]] QString plannedWorktreeName(const QString& repositoryName, qint64 teamId,
                                          const Task& task);

} // namespace ketplus::daemon::teams::branch_names
