// ADR 0114 §8: the stall watchdog. The team is driven by events, so a turn that ends with
// work left and no event to follow would leave the team standing: a developer that stops
// with its task in progress, a leader that ends its turn to wait.
// Some time after a turn ends the watchdog looks at the team and moves it on, each step
// counted against the budget of automatic turns.
#include "teams/TeamCoordinator.h"

#include <QTimer>

namespace ketplus::daemon::teams {
namespace {

// What the team's work looks like; the watchdog starts over when it changes.
QString signatureOf(const Team& team) {
    QStringList parts;
    for (const Task& task : team.tasks) {
        const auto met = std::count_if(task.criteria.cbegin(), task.criteria.cend(),
                                       [](const Criterion& criterion) { return criterion.met; });
        parts.append(QStringLiteral("%1:%2:%3:%4:%5:%6")
                         .arg(task.id, task.state, task.owner, task.pullRequest, task.headCommit)
                         .arg(met));
    }
    parts.append(QString::number(team.decisions.size()));
    return parts.join(QLatin1Char('|'));
}

bool hasWorkLeft(const Team& team) {
    return team.tasks.isEmpty() ||
           std::any_of(team.tasks.cbegin(), team.tasks.cend(),
                       [](const Task& task) { return task.state != state::done; });
}

} // namespace

void TeamCoordinator::watchForStall(const qint64 teamId) {
    QTimer*& timer = stallTimers_[teamId];
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, teamId] { checkStall(teamId); });
    }
    timer->start(stallPeriod_);
}

void TeamCoordinator::checkStall(const qint64 teamId) {
    const Team* team = this->team(teamId);
    // A paused team and a team that waits for the user's answer stand still on purpose.
    if (!team || team->paused || team->openDecisions() > 0 || !hasWorkLeft(*team))
        return;
    Stall& stall = stalls_[teamId];
    if (const QString signature = signatureOf(*team); signature != stall.signature) {
        stall.signature = signature;
        stall.wakes = 0;
        stall.nudged.clear();
    }

    // Who is working. A conversation the runtime no longer knows is not.
    bool anyoneWorking = false;
    QStringList idle;
    for (const Member& member : team->members) {
        const auto conversation = host_->teamConversation(member.sessionId);
        // A member that said what it waits for (team_wait) counts as at work until it is
        // woken to check: it is not nudged and the leader is not woken on its account.
        if (isWaiting(member.sessionId) || (conversation && !conversation->idle))
            anyoneWorking = true;
        else
            idle.append(member.sessionId);
    }

    // A member whose turn ended with its own work open is told so once, whoever else works.
    // The user's Stop is respected: that member is left to the leader and the user.
    QList<TeamMessage> nudges;
    for (const QString& session : idle) {
        const Member* member = team->member(session);
        if (stall.nudged.contains(session) || stall.stopped.contains(session))
            continue;
        QStringList open;
        for (const Task& task : team->tasks) {
            if (task.assignmentHeld)
                continue;
            if (member->role == role::developer && task.owner == session &&
                task.state == state::inProgress)
                open.append(QStringLiteral("%1 \"%2\"").arg(task.id, task.title));
            else if (member->role == role::reviewer && task.state == state::inReview)
                open.append(QStringLiteral("%1 \"%2\"").arg(task.id, task.title));
        }
        if (open.isEmpty())
            continue;
        stall.nudged.insert(session);
        const bool reviewer = member->role == role::reviewer;
        nudges.append(
            {.teamId = team->id,
             .teamName = team->name,
             .toSession = session,
             .kind = QStringLiteral("nudge"),
             .body =
                 QStringList{
                     QStringLiteral("Team %1: your turn ended and this is still %2: %3.")
                         .arg(team->name,
                              reviewer ? QStringLiteral("waiting for your review")
                                       : QStringLiteral("in progress with you"),
                              open.join(QStringLiteral(", "))),
                     reviewer
                         ? QStringLiteral("Review it now and record the verdict on the task.")
                         : QStringLiteral(
                               "Go on with it now. When it is finished, open its pull request "
                               "and set the task to in_review with team_task_update. If you "
                               "cannot go on, block the task with what you wait for, so the "
                               "leader can act; do not end your turn with the task in progress "
                               "and nothing said.")}
                     .join(QLatin1Char('\n'))});
    }
    for (const TeamMessage& nudge : nudges)
        if (const Team* current = this->team(teamId))
            wake(*current, nudge);
    // Whoever works or was nudged ends a turn later, and the watch starts again there.
    if (anyoneWorking || !nudges.isEmpty())
        return;

    team = this->team(teamId);
    const Member* leader = team ? team->memberWithRole(role::leader) : nullptr;
    if (!leader)
        return;
    QStringList lines;
    int left = 0;
    for (const Task& task : team->tasks) {
        if (task.state == state::done)
            continue;
        ++left;
        const Member* owner = task.owner.isEmpty() ? nullptr : team->member(task.owner);
        lines.append(QStringLiteral("- %1 \"%2\" (%3): %4")
                         .arg(task.id, task.title, task.state,
                              !owner ? QStringLiteral("nobody owns it")
                              : stall.stopped.contains(task.owner)
                                  ? QStringLiteral("%1, stopped by the user").arg(owner->name)
                                  : QStringLiteral("%1, idle").arg(owner->name)));
    }
    const QString summary =
        team->tasks.isEmpty()
            ? QStringLiteral("Nobody on the team is working and no task is planned yet.")
            : QStringLiteral("Nobody on the team is working and %1 of %2 tasks are not done.")
                  .arg(left)
                  .arg(team->tasks.size());

    // The leader is woken while that may still help; a leader the user stopped is not.
    if (stall.wakes < maxStallWakes && !stall.stopped.contains(leader->sessionId)) {
        ++stall.wakes;
        QStringList body{QStringLiteral("Team %1 has stopped. %2").arg(team->name, summary)};
        body << lines << QString{}
             << QStringLiteral(
                    "Move it on in this turn: plan or assign what nobody owns, hand over a "
                    "task whose owner cannot go on, tell an owner what it needs, or block a "
                    "task with what it waits for. If you wait for something outside the "
                    "team, check it now. If only the user can unblock the team, raise a "
                    "decision with team_decision_raise. Ending your turn without one of "
                    "these leaves the team standing.")
             << QStringLiteral("This is wake %1 of %2; after that the user is asked.")
                    .arg(stall.wakes)
                    .arg(maxStallWakes);
        wake(*team, {.teamId = team->id,
                     .teamName = team->name,
                     .toSession = leader->sessionId,
                     .kind = QStringLiteral("stall"),
                     .body = body.join(QLatin1Char('\n'))});
        return;
    }

    // Still standing: the user decides, on the team page.
    Team* stuck = mutableTeam(teamId);
    if (!stuck)
        return;
    const Team before = *stuck;
    raiseDecision(
        *stuck, QStringLiteral("stalled"), QStringLiteral("The team has stopped. Move it on?"),
        (QStringList{summary} << lines).join(QLatin1Char('\n')),
        {wakeLeaderChoice, pauseTeamChoice}, wakeLeaderChoice, {}, QStringLiteral("daemon"));
    if (commit(*stuck, before, QStringLiteral("daemon"),
               {{.kind = QStringLiteral("stalled"),
                 .text = QStringLiteral("Stopped: %1").arg(summary)}}))
        qWarning("Unable to record that team %lld has stopped.", static_cast<long long>(teamId));
}

} // namespace ketplus::daemon::teams
