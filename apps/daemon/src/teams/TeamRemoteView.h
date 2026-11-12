#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus::daemon::teams::remote {

// ADR 0116 §6: what a paired phone reads of a team. Built from the documents the local
// protocol returns (TeamCoordinator::teamJson, updatesJson, taskPageJson), without side
// effects. Nothing here carries a local path or a tag: a repository and a worktree are named
// by the opaque catalog ids `Ids` gives.

// The plaintext of one result; sealed, it stays within the mailbox's 512 KiB.
inline constexpr qsizetype maximumResultBytes = 384 * 1024;
inline constexpr qsizetype maximumDirectoryEntries = 100;
inline constexpr qsizetype maximumMembers = 7;
inline constexpr qsizetype maximumTasks = 200;
inline constexpr qsizetype maximumOpenDecisions = 10;
inline constexpr qsizetype maximumClosedDecisions = 10;
inline constexpr int summaryUpdates = 20;
inline constexpr int maximumPage = 50;

struct Ids final {
    // A repository path to its catalog id.
    std::function<QString(const QString&)> repository;
    // A worktree path to its catalog id.
    std::function<QString(const QString&)> worktree;
};

// One team.list entry.
[[nodiscard]] QJsonObject directoryEntry(const QJsonObject& team, const Ids& ids);
// The summary of team.get, of a change's result and of a stale_team refusal.
[[nodiscard]] QJsonObject summary(const QJsonObject& team, const Ids& ids);
// Updates, newest first, as the local protocol returns them, for the phone.
[[nodiscard]] QJsonArray updates(const QJsonArray& local, const QJsonObject& team);
// Decisions as team.decision.list returns them, for the phone.
[[nodiscard]] QJsonArray decisions(const QJsonArray& local, const QJsonObject& team);
// The task of team.task.get: the summary fields, the description, the criteria, the outside
// paths with the lane each belongs to, the criteria note and one page of history.
[[nodiscard]] QJsonObject taskDetail(const QJsonObject& task, const QJsonObject& team,
                                     const Ids& ids);
// The update kinds the phone marks as a warning; a task moved to blocked is one too.
[[nodiscard]] bool isWarning(const QJsonObject& update);
// A pull request's number from its URL or "#n", or nothing.
[[nodiscard]] std::optional<int> pullRequestNumber(const QString& pullRequest);

// The result within maximumResultBytes: answered decisions, then Updates, then done tasks
// are shed, each flagged "<part>_truncated". Nothing when even that does not fit.
[[nodiscard]] std::optional<QJsonObject> bounded(QJsonObject result);

} // namespace ketplus::daemon::teams::remote
