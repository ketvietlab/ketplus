// T-1: a member woken by its team starts a fresh native context at a task boundary or past
// the team's turn threshold, or when the provider's cache has gone cold, so its context does
// not grow with every wake.
#include "teams/TeamContextReset.h"
#include "teams/TeamCoordinator.h"
#include "teams/TeamRules.h"

namespace ketplus::daemon::teams {

std::optional<TeamCoordinator::ContextResetPlan>
TeamCoordinator::planContextReset(const QString& sessionId, const QString& taskHint,
                                  const QString& thread, const qint64 turnSequence,
                                  const context_reset::ThreadActivity& activity) {
    Team* team = mutableTeamOf(sessionId);
    const Member* member = team ? team->member(sessionId) : nullptr;
    if (!member)
        return std::nullopt;
    const QString task = context_reset::memberTask(*team, *member, taskHint);
    std::optional<ContextMark> mark;
    if (const auto it = team->contexts.constFind(sessionId); it != team->contexts.cend())
        mark = *it;
    const std::optional<ContextMark> was = mark;
    const QString reason =
        context_reset::decide(*team, *member, mark, task, thread, turnSequence, activity);
    if (!reason.isEmpty())
        return ContextResetPlan{.reason = reason,
                                .task = task,
                                .briefing = context_reset::briefing(*team, *member, task)};
    if (mark && mark != was) {
        const Team before = *team;
        team->contexts.insert(sessionId, *mark);
        if (commit(*team, before, QStringLiteral("daemon"), {}))
            qWarning("Unable to store the context of a member of team %lld.",
                     static_cast<long long>(before.id));
    }
    return std::nullopt;
}

void TeamCoordinator::noteContextReset(const QString& sessionId, const QString& task,
                                       const qint64 turnSequence) {
    Team* team = mutableTeamOf(sessionId);
    if (!team)
        return;
    const Team before = *team;
    team->contexts.insert(sessionId, {.task = task, .thread = {}, .since = turnSequence});
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        qWarning("Unable to store the fresh context of a member of team %lld.",
                 static_cast<long long>(before.id));
}

std::optional<TeamError> TeamCoordinator::changeContextReset(Team& team, const QJsonObject& changes,
                                                             QList<Update>& updates) {
    QString mode = team.contextReset;
    int turns = team.contextResetTurns;
    int idleMinutes = team.contextResetIdleMinutes;
    if (changes.contains(QStringLiteral("context_reset")))
        if (auto invalid =
                rules::validContextReset(changes.value(QStringLiteral("context_reset")), &mode))
            return invalid;
    if (changes.contains(QStringLiteral("context_reset_turns")))
        if (auto invalid = rules::validContextResetTurns(
                changes.value(QStringLiteral("context_reset_turns")), &turns))
            return invalid;
    if (changes.contains(QStringLiteral("context_reset_idle_minutes")))
        if (auto invalid = rules::validContextResetIdleMinutes(
                changes.value(QStringLiteral("context_reset_idle_minutes")), &idleMinutes))
            return invalid;
    if (mode == team.contextReset && turns == team.contextResetTurns &&
        idleMinutes == team.contextResetIdleMinutes)
        return std::nullopt;
    updates.append(
        {.kind = QStringLiteral("context_reset"),
         .text =
             (turns > 0
                  ? QStringLiteral("Context reset: %1, threshold %2 turns").arg(mode).arg(turns)
                  : QStringLiteral("Context reset: %1, no turn threshold").arg(mode)) +
             (idleMinutes > 0 ? QStringLiteral(", cold cache after %1 minutes.").arg(idleMinutes)
                              : QStringLiteral(", no cold cache reset.")),
         .taskId = {},
         .actionable = false,
         .data = {{QStringLiteral("from"), team.contextReset},
                  {QStringLiteral("to"), mode},
                  {QStringLiteral("turns_from"), team.contextResetTurns},
                  {QStringLiteral("turns_to"), turns},
                  {QStringLiteral("idle_minutes_from"), team.contextResetIdleMinutes},
                  {QStringLiteral("idle_minutes_to"), idleMinutes}}});
    team.contextReset = mode;
    team.contextResetTurns = turns;
    team.contextResetIdleMinutes = idleMinutes;
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
