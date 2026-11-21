// ADR 0114 §8: what a member waits for outside the team. A member that waits for a build, a
// deployment or another conversation says so with team_wait and is woken to check again;
// a message it sent outside the team that failed is reported back to it. Both are turns the
// team starts, counted against the budget of automatic turns and held while it is paused.
#include "teams/TeamCoordinator.h"

#include <QTimer>

namespace ketplus::daemon::teams {

TeamResult TeamCoordinator::wait(const qint64 teamId, const QString& sessionId, const int minutes,
                                 const QString& reason, const QString& taskId) {
    Team* team = mutableTeam(teamId);
    const Member* member = team ? team->member(sessionId) : nullptr;
    if (!member)
        return {.error = TeamError{QStringLiteral("not_on_team"),
                                   QStringLiteral("This conversation is not on the team.")}};
    if (member->role == role::reviewer)
        return {.error = TeamError{QStringLiteral("not_allowed"),
                                   QStringLiteral("A reviewer is sent its reviews; it does not "
                                                  "wait.")}};
    const QString name = member->name;
    const QString why = reason.simplified();
    if (why.isEmpty())
        return {.error = TeamError{QStringLiteral("invalid_payload"),
                                   QStringLiteral("Say what you wait for.")}};
    if (minutes < 1 || minutes > maxWaitMinutes)
        return {.error = TeamError{
                    QStringLiteral("invalid_payload"),
                    QStringLiteral("Wait between 1 and %1 minutes.").arg(maxWaitMinutes)}};
    if (!taskId.isEmpty() && !team->task(taskId))
        return {.error = TeamError{QStringLiteral("task_not_found"),
                                   QStringLiteral("There is no task %1.").arg(taskId)}};

    const Team before = *team;
    if (auto failed =
            commit(*team, before, sessionId,
                   {{.kind = QStringLiteral("wait"),
                     .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 waits %2 min: %3"))
                                 .arg(name)
                                 .arg(minutes)
                                 .arg(why),
                     .taskId = taskId}}))
        return {.error = failed};

    // One wait per member: a new one replaces the old.
    cancelWait(sessionId);
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, sessionId] { waitEnded(sessionId); });
    waits_.insert(sessionId, {.timer = timer, .reason = why, .taskId = taskId});
    timer->start(minutes * waitMinuteMs_);
    return {.teamId = teamId};
}

void TeamCoordinator::cancelWait(const QString& sessionId) {
    const auto found = waits_.constFind(sessionId);
    if (found == waits_.cend())
        return;
    delete found->timer;
    waits_.erase(found);
}

void TeamCoordinator::waitEnded(const QString& sessionId) {
    const auto found = waits_.constFind(sessionId);
    if (found == waits_.cend())
        return;
    const Wait wait = *found;
    waits_.erase(found);
    wait.timer->deleteLater();
    const Team* team = teamOf(sessionId);
    if (!team)
        return;
    // A member that works again got there by itself; its turn's end starts the watch.
    const qint64 teamId = team->id;
    watchForStall(teamId);
    const auto conversation = host_->teamConversation(sessionId);
    if (!conversation || !conversation->idle)
        return;
    wake(*team,
         {.teamId = team->id,
          .teamName = team->name,
          .toSession = sessionId,
          .kind = QStringLiteral("wait"),
          .taskId = wait.taskId,
          .body = QStringList{QStringLiteral("Team %1: you asked to check again: %2")
                                  .arg(team->name, wait.reason),
                              QStringLiteral("Check it now. Then go on with the work, block the "
                                             "task with what it waits for, or call team_wait "
                                             "again if it is still not there.")}
                      .join(QLatin1Char('\n'))});
}

void TeamCoordinator::outsideMessageFailed(const QString& sessionId, const QString& peer,
                                           const QString& reason) {
    const Team* team = teamOf(sessionId);
    const Member* member = team ? team->member(sessionId) : nullptr;
    // The user's Stop is respected, as the watchdog does.
    if (!member || stalls_.value(team->id).stopped.contains(sessionId))
        return;
    const QString what =
        reason == QStringLiteral("no_reply") ? QStringLiteral("its turn ended without an answer")
        : reason.startsWith(QStringLiteral("turn_")) ? QStringLiteral("its turn did not complete")
        : reason.startsWith(QStringLiteral("expired")) ||
                reason == QStringLiteral("no_reply_in_time")
            ? QStringLiteral("it was not answered in time")
            : QStringLiteral("it could not be delivered");
    wake(*team,
         {.teamId = team->id,
          .teamName = team->name,
          .toSession = sessionId,
          .kind = QStringLiteral("message_failed"),
          .body =
              QStringList{
                  QStringLiteral("Team %1: your message to conversation %2 failed: %3 (%4). "
                                 "No answer to it will arrive.")
                      .arg(team->name, peer, what, reason),
                  member->role == role::leader
                      ? QStringLiteral("Decide now: send it again, go on without the answer, "
                                       "or raise a decision with team_decision_raise if only "
                                       "the user can unblock it.")
                      : QStringLiteral("Decide now: send it again, go on without the answer, "
                                       "or block your task with what you wait for so the "
                                       "leader can act.")}
                  .join(QLatin1Char('\n'))});
}

} // namespace ketplus::daemon::teams
