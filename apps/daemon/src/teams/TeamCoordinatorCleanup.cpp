// ADR 0114 §10 (T-23): the removal of several done worktrees as one background run. The request
// answers at once; the worktrees then go one after another through removeWorktree, so each keeps
// every check a single removal has, while the host does the work off its event loop. What waits,
// what is being removed and what the last run did stay in memory, per team: the team page reads
// them again from team.storage.get, and a restarted daemon has no run that could hang.
#include "teams/TeamCoordinator.h"

#include <QDateTime>
#include <QJsonArray>
#include <QPointer>

namespace ketplus::daemon::teams {
namespace {

constexpr qsizetype maximumWorktrees = 200;

QJsonObject reason(const QString& taskId, const QString& key, const QString& text) {
    return {{QStringLiteral("task_id"), taskId}, {key, text}};
}

} // namespace

QJsonObject TeamCoordinator::cleanupJson(const qint64 teamId) const {
    const auto it = cleanups_.constFind(teamId);
    if (it == cleanups_.cend())
        return {{QStringLiteral("run"), 0},
                {QStringLiteral("active"), QJsonArray{}},
                {QStringLiteral("last"), QJsonValue::Null}};
    QJsonArray active;
    if (!it->removing.isEmpty())
        active.append(QJsonObject{{QStringLiteral("task_id"), it->removing},
                                  {QStringLiteral("state"), QStringLiteral("removing")}});
    for (const QString& taskId : it->waiting)
        active.append(QJsonObject{{QStringLiteral("task_id"), taskId},
                                  {QStringLiteral("state"), QStringLiteral("queued")}});
    return {{QStringLiteral("run"), it->run},
            {QStringLiteral("active"), active},
            {QStringLiteral("last"),
             it->last.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(it->last)}};
}

void TeamCoordinator::cleanupWorktrees(const qint64 teamId, const QStringList& taskIds,
                                       const QString& actor, CleanupAnswer done) {
    if (!team(teamId)) {
        done(TeamError{QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists.")},
             {});
        return;
    }
    if (actor != QStringLiteral("user")) {
        done(TeamError{QStringLiteral("role_forbidden"),
                       QStringLiteral("Only you remove a worktree, from the team page.")},
             {});
        return;
    }
    QStringList ids;
    for (const QString& id : taskIds)
        if (!id.trimmed().isEmpty() && !ids.contains(id.trimmed()))
            ids.append(id.trimmed());
    if (ids.isEmpty() || ids.size() > maximumWorktrees) {
        done(TeamError{QStringLiteral("invalid_payload"),
                       QStringLiteral("Name one to %1 worktrees to remove.").arg(maximumWorktrees)},
             {});
        return;
    }
    Cleanup& cleanup = cleanups_[teamId];
    // A run that finished is replaced by the next, and its summary with it.
    if (cleanup.removing.isEmpty() && cleanup.waiting.isEmpty()) {
        cleanup = {};
        cleanup.run = ++cleanupRuns_;
    }
    for (const QString& id : ids) {
        if (cleanup.removing == id || cleanup.waiting.contains(id))
            continue;
        if (const auto refused = removalRefusal(teamId, id, actor)) {
            cleanup.kept.append({.taskId = id, .text = refused->message});
            continue;
        }
        cleanup.waiting.append(id);
    }
    if (cleanup.waiting.isEmpty() && cleanup.removing.isEmpty()) {
        // Every one was refused: there is nothing to wait for.
        finishCleanup(teamId);
        done(std::nullopt, cleanupJson(teamId));
        return;
    }
    emit storageChanged(teamId);
    // The first removal starts before the answer, so the answer says which is being removed.
    pumpCleanup(teamId);
    done(std::nullopt, cleanupJson(teamId));
}

// Starts the next removal when none runs; the one that ends starts the one after it.
void TeamCoordinator::pumpCleanup(const qint64 teamId) {
    auto it = cleanups_.find(teamId);
    if (it == cleanups_.end() || !it->removing.isEmpty())
        return;
    if (!team(teamId)) {
        cleanups_.erase(it);
        return;
    }
    while (!it->waiting.isEmpty()) {
        const QString taskId = it->waiting.takeFirst();
        // At its turn: a conversation may have started in the folder since it was asked for.
        if (const auto refused = removalRefusal(teamId, taskId, QStringLiteral("user"))) {
            it->kept.append({.taskId = taskId, .text = refused->message});
            continue;
        }
        const Task* task = team(teamId)->task(taskId);
        const qint64 bytes = worktreeSizes_.value(task->worktree, -1);
        it->removing = taskId;
        emit storageChanged(teamId);
        removeWorktree(teamId, taskId, QStringLiteral("user"),
                       [guard = QPointer<TeamCoordinator>(this), teamId, taskId,
                        bytes](const TeamResult& result, const QString& note) {
                           if (!guard)
                               return;
                           auto now = guard->cleanups_.find(teamId);
                           if (now == guard->cleanups_.end())
                               return;
                           now->removing.clear();
                           if (result.ok())
                               now->removed.append(
                                   {.taskId = taskId, .text = note, .bytes = bytes});
                           else
                               now->kept.append({.taskId = taskId, .text = result.error->message});
                           guard->pumpCleanup(teamId);
                       });
        return;
    }
    finishCleanup(teamId);
}

void TeamCoordinator::finishCleanup(const qint64 teamId) {
    auto it = cleanups_.find(teamId);
    if (it == cleanups_.end())
        return;
    QJsonArray removed;
    QJsonArray notes;
    QJsonArray kept;
    qint64 freed = 0;
    bool exact = true;
    for (const CleanupItem& item : std::as_const(it->removed)) {
        removed.append(QJsonObject{{QStringLiteral("task_id"), item.taskId},
                                   {QStringLiteral("size_bytes"),
                                    item.bytes >= 0 ? QJsonValue(item.bytes) : QJsonValue()}});
        if (item.bytes >= 0)
            freed += item.bytes;
        else
            exact = false;
        if (!item.text.isEmpty())
            notes.append(reason(item.taskId, QStringLiteral("note"), item.text));
    }
    for (const CleanupItem& item : std::as_const(it->kept))
        kept.append(reason(item.taskId, QStringLiteral("reason"), item.text));
    it->last = {{QStringLiteral("run"), it->run},
                {QStringLiteral("removed"), removed},
                {QStringLiteral("kept"), kept},
                {QStringLiteral("notes"), notes},
                {QStringLiteral("freed_bytes"), freed},
                {QStringLiteral("freed_exact"), exact},
                {QStringLiteral("finished_at"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    it->removed.clear();
    it->kept.clear();
    emit storageChanged(teamId);
}

} // namespace ketplus::daemon::teams
