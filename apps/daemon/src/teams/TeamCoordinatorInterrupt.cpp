// ADR 0114 §11: the leader interrupts a member. The record and the hourly limit live here; the
// host stops the turn and posts the message (SessionHostTeamInterrupt.cpp).
#include "teams/TeamCoordinator.h"

#include <QDateTime>

namespace ketplus::daemon::teams {

TeamResult TeamCoordinator::noteLeaderInterrupt(const qint64 teamId, const QString& sessionId,
                                                const QString& reason, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = TeamError{QStringLiteral("team_not_found"),
                                   QStringLiteral("The team no longer exists.")}};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    const Member* leader = team->member(actor);
    if (!leader || leader->role != role::leader)
        return {.error = TeamError{QStringLiteral("role_forbidden"),
                                   QStringLiteral("Only the leader interrupts a member.")}};
    const Member* member = team->member(sessionId);
    if (!member)
        return {.error = TeamError{QStringLiteral("not_member"),
                                   QStringLiteral("That conversation is not on the team.")}};
    if (member->role == role::leader)
        return {.error = TeamError{QStringLiteral("not_allowed"),
                                   QStringLiteral("The leader cannot interrupt itself.")}};
    if (team->paused)
        return {.error = TeamError{QStringLiteral("team_paused"),
                                   QStringLiteral("The team is paused; resume it first.")}};
    const QString why = reason.trimmed();
    if (why.isEmpty() || why.size() > 64)
        return {.error = TeamError{QStringLiteral("invalid_payload"),
                                   QStringLiteral("Give a short reason code, at most 64 "
                                                  "characters.")}};
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<QDateTime>& times = interrupts_[sessionId];
    times.removeIf([&now](const QDateTime& at) { return at.secsTo(now) >= 3600; });
    if (times.size() >= maximumInterruptsPerHour)
        return {.error = TeamError{QStringLiteral("interrupt_limit"),
                                   QStringLiteral("%1 was interrupted %2 times in the last hour. "
                                                  "Send a normal message with agents_send.")
                                       .arg(member->name)
                                       .arg(maximumInterruptsPerHour)}};
    const QString name = member->name;
    const QString text =
        say(team->language, QT_TRANSLATE_NOOP("Team", "leader interrupted %1: %2")).arg(name, why);
    const QJsonObject data{{QStringLiteral("session_id"), sessionId},
                           {QStringLiteral("reason"), why}};
    for (Task& task : team->tasks)
        if (task.owner == sessionId && task.state != state::done)
            addHistory(task, actor, QStringLiteral("interrupted"), text, data);
    QList<Update> updates{
        {.kind = QStringLiteral("member_interrupted"), .text = text, .data = data}};
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    times.append(now);
    return {.teamId = teamId};
}

} // namespace ketplus::daemon::teams
