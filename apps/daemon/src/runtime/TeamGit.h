#pragma once

// What a team asks git (ADR 0114): free functions over copies of a team and its tasks, so a
// thread other than the session runtime's can run them.
#include "teams/TeamCoordinator.h"

#include <optional>

namespace ketplus::daemon::team_git {

// A short git call in `directory`; nothing when git failed or did not answer in time.
[[nodiscard]] std::optional<QString> output(const QString& directory, const QStringList& arguments);
// The commit the task's branch points at in the team's repository; empty when unknown.
[[nodiscard]] QString taskCommit(const teams::Team& team, const teams::Task& task);
// Paths changed in the task's worktree against its base, committed or not.
[[nodiscard]] std::optional<QStringList> changedPaths(const teams::Team& team,
                                                      const teams::Task& task);
// Whether the integration branch contains the task's branch; nothing when git failed.
[[nodiscard]] std::optional<bool> isMerged(const teams::Team& team, const teams::Task& task);
// The task's branch against its base on the integration branch; nothing when git failed.
[[nodiscard]] std::optional<teams::TaskDiff> taskDiff(const teams::Team& team,
                                                      const teams::Task& task);
// The answers to one check, by task id.
[[nodiscard]] QHash<QString, teams::TaskFacts>
facts(const teams::Team& team, const QList<teams::Task>& tasks, teams::TaskQuestion question);

} // namespace ketplus::daemon::team_git
