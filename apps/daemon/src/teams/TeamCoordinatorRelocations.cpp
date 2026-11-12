// ADR 0114 §7: a member handed out of a task's worktree leaves it. T-32: the switch asks git
// off the daemon's thread through askPointConversationAt, as the outbox's switch does, and
// one conversation's switches run one after another, each deciding where to go once the one
// before it is stored.
#include "teams/TeamCoordinator.h"

#include <QDir>
#include <QPointer>

#include <utility>

namespace ketplus::daemon::teams {

std::optional<QString> TeamCoordinator::relocationHome(const Team& team,
                                                       const Relocation& move) const {
    const QString from = QDir::cleanPath(move.from);
    const auto conversation = host_->teamConversation(move.session);
    // Settled without a move: the conversation is gone or stands elsewhere already, or the
    // task came back to this member.
    if (!team.member(move.session) || !conversation ||
        QDir::cleanPath(conversation->worktree) != from)
        return std::nullopt;
    // It goes to the task it works on, else to another task of its own, else to the
    // repository.
    QString home = team.repository;
    bool working = false;
    for (const Task& task : std::as_const(team.tasks)) {
        if (task.owner != move.session || task.state == state::done || task.worktree.isEmpty())
            continue;
        if (QDir::cleanPath(task.worktree) == from)
            return std::nullopt;
        if (!working && (task.state == state::inProgress || home == team.repository)) {
            home = task.worktree;
            working = task.state == state::inProgress;
        }
    }
    return home;
}

QString TeamCoordinator::taskWorktreeRepository(const QString& worktree) const {
    const QString path = QDir::cleanPath(worktree);
    for (const Team& team : teams_)
        for (const Task& task : team.tasks)
            if (!task.worktree.isEmpty() && QDir::cleanPath(task.worktree) == path)
                return team.repository;
    return {};
}

void TeamCoordinator::whenConversationFree(const QString& sessionId, std::function<void()> job) {
    if (auto it = conversationJobs_.find(sessionId); it != conversationJobs_.end()) {
        it->append(std::move(job));
        return;
    }
    conversationJobs_.insert(sessionId, {});
    job();
}

void TeamCoordinator::conversationFree(const QString& sessionId) {
    auto it = conversationJobs_.find(sessionId);
    if (it == conversationJobs_.end())
        return;
    if (it->isEmpty()) {
        conversationJobs_.erase(it);
        return;
    }
    const std::function<void()> next = it->takeFirst();
    next();
}

void TeamCoordinator::settleRelocations(const qint64 teamId, const QString& sessionId,
                                        PointDone done) {
    whenConversationFree(sessionId, [this, teamId, sessionId, done = std::move(done)] {
        settleStep(teamId, sessionId, [this, sessionId, done](const QString& waiting) {
            if (done)
                done(waiting);
            conversationFree(sessionId);
        });
    });
}

// Forgets the moves that are settled without a switch, then switches for the first one left.
// A switch that worked is forgotten and the rest is looked at again; one that failed stays
// stored, with the moves after it, and is tried again when the member's turn ends.
void TeamCoordinator::settleStep(const qint64 teamId, const QString& sessionId, PointDone done) {
    Team* team = mutableTeam(teamId);
    std::optional<Relocation> next;
    QString home;
    if (team && !team->relocations.isEmpty()) {
        QList<Relocation> kept;
        for (const Relocation& move : std::as_const(team->relocations)) {
            if (move.session != sessionId) {
                kept.append(move);
                continue;
            }
            const std::optional<QString> target = relocationHome(*team, move);
            if (!target)
                continue;
            kept.append(move);
            if (!next) {
                next = move;
                home = *target;
            }
        }
        if (kept.size() != team->relocations.size()) {
            const Team before = *team;
            team->relocations = kept;
            // A move that stays stored is tried again and found settled.
            if (commit(*team, before, QStringLiteral("daemon"), {}))
                qWarning("Unable to forget a settled move out of a handed-over worktree.");
        }
    }
    if (!next) {
        done({});
        return;
    }
    host_->askPointConversationAt(
        sessionId, home,
        [self = QPointer<TeamCoordinator>(this), teamId, sessionId, move = *next,
         done = std::move(done)](const QString& error) {
            if (!self)
                return;
            if (!error.isEmpty()) {
                done(error);
                return;
            }
            if (Team* current = self->mutableTeam(teamId); current) {
                const Team before = *current;
                if (current->relocations.removeAll(move) > 0 &&
                    self->commit(*current, before, QStringLiteral("daemon"), {}))
                    qWarning("Unable to forget a settled move out of a handed-over worktree.");
            }
            self->settleStep(teamId, sessionId, done);
        });
}

void TeamCoordinator::settleStoredRelocations() {
    for (const qint64 id : teams_.keys()) {
        const Team* team = this->team(id);
        if (!team)
            continue;
        QStringList sessions;
        for (const Relocation& move : team->relocations)
            if (!sessions.contains(move.session))
                sessions.append(move.session);
        for (const QString& session : std::as_const(sessions))
            settleRelocations(id, session, {});
    }
}

} // namespace ketplus::daemon::teams
