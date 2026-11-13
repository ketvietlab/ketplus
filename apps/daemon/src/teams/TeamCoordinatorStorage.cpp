// ADR 0114 §10: the disk under a team's worktrees, and the removal of a done task's worktree.
// Both are the user's, from the desktop's team page: there is no agent tool for either, and
// nothing here pauses a team or removes a worktree on its own.
#include "teams/TeamCoordinator.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <memory>

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

QString busy(const QString& state, const bool idle, const bool approvalsPending) {
    if (approvalsPending)
        return QStringLiteral("waits for your approval");
    if (state == QStringLiteral("waiting_for_input"))
        return QStringLiteral("waits for your answer");
    if (!idle)
        return QStringLiteral("is running a turn");
    return {};
}
} // namespace storage

QList<const Task*> TeamCoordinator::doneWorktrees(const Team& team) {
    QList<const Task*> done;
    for (const Task& task : team.tasks)
        // T-90: never the main checkout of the repository the task worked in.
        if (task.state == state::done && !task.worktree.isEmpty() &&
            QFileInfo(task.worktree).isDir() &&
            canonical(task.worktree) != canonical(taskScope(team, task).repository))
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
    const auto refuse = [&](const TeamError& error) {
        done({.error = error, .teamId = teamId}, {});
    };
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
    // T-90: the worktree belongs to the repository the task worked in.
    const QString repository = taskScope(*team, *task).repository;
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return refuse(notRemoved(QStringLiteral("%1 has no worktree on disk.").arg(taskId)));
    if (canonical(path) == canonical(repository))
        return refuse(notRemoved(QStringLiteral("%1 is the repository's main checkout, which is "
                                                "never removed.")
                                     .arg(path)));
    if (const QString busy = host_->conversationRunningIn(path); !busy.isEmpty())
        return refuse(notRemoved(
            QStringLiteral("%1 in %2. Remove the worktree once it is idle.").arg(busy, path)));
    if (worktreeRemovals_.contains(path))
        return refuse(notRemoved(QStringLiteral("%1 is being removed already.").arg(path)));
    worktreeRemovals_.insert(path);
    host_->askRemoveWorktree(repository, path,
                             [guard = QPointer<TeamCoordinator>(this), teamId, taskId, path,
                              done = std::move(done)](const QString& error) {
                                 if (!guard)
                                     return;
                                 guard->worktreeRemovals_.remove(path);
                                 if (!error.isEmpty()) {
                                     done({.error = notRemoved(error), .teamId = teamId}, {});
                                     return;
                                 }
                                 guard->worktreeSizes_.remove(path);
                                 guard->worktreeRemoved(teamId, taskId, path, std::move(done));
                             });
}

// The folder is gone. An idle conversation still in it, such as the developer of the team's
// last task, is moved to the main checkout; the task forgets the worktree once every move has
// answered, and its history says what happened to each conversation.
void TeamCoordinator::worktreeRemoved(const qint64 teamId, const QString& taskId,
                                      const QString& path, RemovalAnswer done) {
    const Team* team = this->team(teamId);
    if (!team) {
        done({.error = std::nullopt, .teamId = teamId}, {});
        return;
    }
    struct Moves final {
        int waiting{0};
        QStringList moved;
        QStringList stayed;
    };
    // T-90: the main checkout of the repository the task worked in.
    const Task* task = team->task(taskId);
    const QString home = task ? taskScope(*team, *task).repository : team->repository;
    const QList<FolderConversation> inFolder = host_->conversationsIn(path);
    auto moves = std::make_shared<Moves>();
    moves->waiting = static_cast<int>(inFolder.size());
    const auto finish = [guard = QPointer<TeamCoordinator>(this), teamId, taskId, path, moves,
                         done = std::move(done)] {
        if (!guard)
            return;
        Team* now = guard->mutableTeam(teamId);
        Task* removed = now ? now->task(taskId) : nullptr;
        const QString note =
            moves->stayed.isEmpty()
                ? QString{}
                : QStringLiteral("%1 could not be moved to the main checkout and still points at "
                                 "the removed folder. Open it on another folder before its next "
                                 "turn.")
                      .arg(moves->stayed.join(QStringLiteral("; ")));
        if (!removed || removed->worktree != path) {
            done({.error = std::nullopt, .teamId = teamId}, note);
            return;
        }
        const Team before = *now;
        removed->worktree.clear();
        QString text = QStringLiteral("You removed the worktree %1.").arg(path);
        if (!moves->moved.isEmpty())
            text += QStringLiteral(" %1 now %2 in the main checkout.")
                        .arg(moves->moved.join(QStringLiteral(", ")), moves->moved.size() == 1
                                                                          ? QStringLiteral("works")
                                                                          : QStringLiteral("work"));
        if (!note.isEmpty())
            text += QLatin1Char(' ') + note;
        const QJsonObject data{
            {QStringLiteral("path"), path},
            {QStringLiteral("moved"), QJsonArray::fromStringList(moves->moved)},
            {QStringLiteral("not_moved"), QJsonArray::fromStringList(moves->stayed)}};
        addHistory(*removed, QStringLiteral("user"), QStringLiteral("worktree_removed"), text,
                   data);
        const auto failed = guard->commit(*now, before, QStringLiteral("user"),
                                          {{.kind = QStringLiteral("worktree_removed"),
                                            .text = QStringLiteral("%1: %2").arg(taskId, text),
                                            .taskId = taskId,
                                            .actionable = false,
                                            .data = data}});
        done({.error = failed, .teamId = teamId}, note);
    };
    if (inFolder.isEmpty()) {
        finish();
        return;
    }
    for (const FolderConversation& conversation : inFolder) {
        const QString name = QStringLiteral("“%1”").arg(
            conversation.title.isEmpty() ? conversation.sessionId : conversation.title);
        host_->askPointConversationAt(
            conversation.sessionId, home, [moves, name, finish](const QString& error) {
                if (error.isEmpty())
                    moves->moved.append(name);
                else
                    moves->stayed.append(QStringLiteral("%1 (%2)").arg(name, error));
                if (--moves->waiting == 0)
                    finish();
            });
    }
}

} // namespace ketplus::daemon::teams
