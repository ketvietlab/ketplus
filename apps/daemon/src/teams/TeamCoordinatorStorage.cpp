// ADR 0114 §10: the disk under a team's worktrees, and the removal of a done task's worktree.
// Both are the user's, from the desktop's team page: there is no agent tool for either, and
// nothing here pauses a team or removes a worktree on its own.
#include "teams/TeamCoordinator.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

namespace ketplus::daemon::teams {
namespace {

TeamError notRemoved(const QString& message) {
    return {QStringLiteral("worktree_not_removed"), message};
}

QString canonical(const QString& path) {
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? QDir::cleanPath(path) : resolved;
}

} // namespace

namespace storage {
QString level(const std::optional<qint64> freeBytes) {
    if (!freeBytes)
        return {};
    if (*freeBytes < stopBytes)
        return QStringLiteral("stop");
    if (*freeBytes < warnBytes)
        return QStringLiteral("low");
    return {};
}
} // namespace storage

QList<const Task*> TeamCoordinator::doneWorktrees(const Team& team) {
    QList<const Task*> done;
    const QString main = canonical(team.repository);
    for (const Task& task : team.tasks)
        if (task.state == state::done && !task.worktree.isEmpty() &&
            QFileInfo(task.worktree).isDir() && canonical(task.worktree) != main)
            done.append(&task);
    return done;
}

void TeamCoordinator::storage(const qint64 teamId, StorageAnswer done) {
    const Team* team = this->team(teamId);
    if (!team) {
        done(TeamError{QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists.")},
             {});
        return;
    }
    // Only a worktree not measured yet is walked; the free space is read every time.
    QStringList unmeasured;
    for (const Task* task : doneWorktrees(*team))
        if (!worktreeSizes_.contains(task->worktree))
            unmeasured.append(task->worktree);
    host_->askStorage(
        team->repository, unmeasured,
        [guard = QPointer<TeamCoordinator>(this), teamId,
         done = std::move(done)](StorageFacts facts) {
            if (guard)
                for (auto size = facts.sizes.cbegin(); size != facts.sizes.cend(); ++size)
                    guard->worktreeSizes_.insert(size.key(), size.value());
            const Team* now = guard ? guard->team(teamId) : nullptr;
            if (!now) {
                done(TeamError{QStringLiteral("team_not_found"),
                               QStringLiteral("The team no longer exists.")},
                     {});
                return;
            }
            QJsonArray worktrees;
            for (const Task* task : doneWorktrees(*now)) {
                const auto size = guard->worktreeSizes_.constFind(task->worktree);
                worktrees.append(QJsonObject{
                    {QStringLiteral("task_id"), task->id},
                    {QStringLiteral("title"), task->title},
                    {QStringLiteral("branch"), task->branch},
                    {QStringLiteral("path"), task->worktree},
                    {QStringLiteral("size_bytes"),
                     size == guard->worktreeSizes_.cend() ? QJsonValue() : QJsonValue(*size)}});
            }
            done(std::nullopt,
                 QJsonObject{{QStringLiteral("free_bytes"),
                              facts.freeBytes ? QJsonValue(*facts.freeBytes) : QJsonValue()},
                             {QStringLiteral("level"), storage::level(facts.freeBytes)},
                             {QStringLiteral("warn_bytes"), storage::warnBytes},
                             {QStringLiteral("stop_bytes"), storage::stopBytes},
                             {QStringLiteral("worktrees"), worktrees}});
        });
}

void TeamCoordinator::removeWorktree(const qint64 teamId, const QString& taskId,
                                     const QString& actor, RemovalAnswer done) {
    const auto refuse = [&](const TeamError& error) { done({.error = error, .teamId = teamId}); };
    const Team* team = this->team(teamId);
    if (!team)
        return refuse(
            {QStringLiteral("team_not_found"), QStringLiteral("The team no longer exists.")});
    // Agents have no tool for it, and the daemon serves team.* only to the desktop; this
    // keeps any other caller out as well.
    if (actor != QStringLiteral("user"))
        return refuse({QStringLiteral("role_forbidden"),
                       QStringLiteral("Only you remove a worktree, from the team page.")});
    const Task* task = team->task(taskId);
    if (!task)
        return refuse(
            {QStringLiteral("task_not_found"), QStringLiteral("There is no task %1.").arg(taskId)});
    if (task->state != state::done)
        return refuse(notRemoved(
            QStringLiteral("%1 is not done; only a done task's worktree is removed.").arg(taskId)));
    const QString path = task->worktree;
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return refuse(notRemoved(QStringLiteral("%1 has no worktree on disk.").arg(taskId)));
    if (canonical(path) == canonical(team->repository))
        return refuse(notRemoved(QStringLiteral("%1 is the repository's main checkout, which is "
                                                "never removed.")
                                     .arg(path)));
    if (const QString running = host_->conversationRunningIn(path); !running.isEmpty())
        return refuse(notRemoved(QStringLiteral("“%1” is running a turn in %2. Remove the "
                                                "worktree once the turn has ended.")
                                     .arg(running, path)));
    if (worktreeRemovals_.contains(path))
        return refuse(notRemoved(QStringLiteral("%1 is being removed already.").arg(path)));
    worktreeRemovals_.insert(path);
    host_->askRemoveWorktree(
        team->repository, path,
        [guard = QPointer<TeamCoordinator>(this), teamId, taskId, path,
         done = std::move(done)](const QString& error) {
            if (!guard)
                return;
            guard->worktreeRemovals_.remove(path);
            if (error.isEmpty())
                guard->worktreeSizes_.remove(path);
            if (!error.isEmpty()) {
                done({.error = notRemoved(error), .teamId = teamId});
                return;
            }
            // The folder is gone: the task forgets it, and says so in its history.
            Team* now = guard->mutableTeam(teamId);
            Task* removed = now ? now->task(taskId) : nullptr;
            if (!removed || removed->worktree != path) {
                done({.error = std::nullopt, .teamId = teamId});
                return;
            }
            const Team before = *now;
            removed->worktree.clear();
            // An idle conversation still in the folder, such as the developer of the team's last
            // task, goes on in the main checkout instead of a folder that is gone.
            const QStringList moved = guard->host_->conversationsIn(path);
            for (const QString& sessionId : moved)
                guard->host_->askPointConversationAt(sessionId, now->repository,
                                                     [](const QString&) {});
            const QString text =
                moved.isEmpty()
                    ? QStringLiteral("You removed the worktree %1.").arg(path)
                    : QStringLiteral("You removed the worktree %1; %2 now in the main checkout.")
                          .arg(path,
                               moved.size() == 1
                                   ? QStringLiteral("its conversation works")
                                   : QStringLiteral("%1 conversations work").arg(moved.size()));
            const QJsonObject data{{QStringLiteral("path"), path}};
            guard->addHistory(*removed, QStringLiteral("user"), QStringLiteral("worktree_removed"),
                              text, data);
            const auto failed = guard->commit(*now, before, QStringLiteral("user"),
                                              {{.kind = QStringLiteral("worktree_removed"),
                                                .text = QStringLiteral("%1: %2").arg(taskId, text),
                                                .taskId = taskId,
                                                .actionable = false,
                                                .data = data}});
            done({.error = failed, .teamId = teamId});
        });
}

} // namespace ketplus::daemon::teams
