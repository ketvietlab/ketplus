// ADR 0114 §7 and open point 2: what starts a member's turn, the budget of automatic turns,
// the messages held while a team is paused, and the leader's digest.
#include "storage/LocalJournal.h"
#include "teams/TeamCoordinator.h"

#include <QTimer>

namespace ketplus::daemon::teams {
namespace {

QJsonObject heldJson(const TeamMessage& message) {
    return {{QStringLiteral("to"), message.toSession},
            {QStringLiteral("from"), message.fromSession},
            {QStringLiteral("kind"), message.kind},
            {QStringLiteral("task_id"), message.taskId},
            {QStringLiteral("body"), message.body},
            {QStringLiteral("expect_reply"), message.expectReply},
            {QStringLiteral("id"), message.id}};
}

TeamMessage heldMessage(const Team& team, const QJsonObject& json) {
    return {.teamId = team.id,
            .teamName = team.name,
            .toSession = json.value(QStringLiteral("to")).toString(),
            .fromSession = json.value(QStringLiteral("from")).toString(),
            .kind = json.value(QStringLiteral("kind")).toString(),
            .taskId = json.value(QStringLiteral("task_id")).toString(),
            .body = json.value(QStringLiteral("body")).toString(),
            .expectReply = json.value(QStringLiteral("expect_reply")).toBool(),
            .id = json.value(QStringLiteral("id")).toString()};
}

// Every update since the last digest, grouped by task, the actionable ones marked.
QString digestText(const Team& team, const QList<TeamUpdateRecord>& updates) {
    QStringList order;
    QHash<QString, QStringList> lines;
    for (const TeamUpdateRecord& update : updates) {
        const QString key = update.taskId;
        if (!lines.contains(key))
            order.append(key);
        lines[key].append(QStringLiteral("%1 %2").arg(
            update.actionable ? QStringLiteral("- [needs you]") : QStringLiteral("-"),
            update.text.left(400)));
    }
    QStringList text{
        QStringLiteral("Team %1: what changed since your last digest.").arg(team.name)};
    for (const QString& key : order) {
        const Task* task = key.isEmpty() ? nullptr : team.task(key);
        text << QString{}
             << (key.isEmpty() ? QStringLiteral("Team")
                 : task ? QStringLiteral("%1 \"%2\" (%3)").arg(task->id, task->title, task->state)
                        : key);
        text << lines.value(key);
    }
    text << QString{}
         << QStringLiteral("Act on the [needs you] lines: plan, assign, hand over, change a "
                           "lane or raise a decision. Read a task with team_tasks.");
    return text.join(QLatin1Char('\n'));
}

} // namespace

void TeamCoordinator::wake(const Team& snapshot, const TeamMessage& message) {
    const qint64 teamId = snapshot.id;
    // A turn the team starts never runs in a worktree its member was handed out of.
    (void)settleRelocations(teamId, message.toSession);
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    const Team before = *team;
    QList<Update> updates;
    if (!team->paused && team->automaticTurns >= team->turnBudget) {
        // The budget is used up: pause once and ask (ADR 0114, open point 2).
        team->paused = true;
        team->pausedReason = QStringLiteral("budget");
        const QString more = QStringLiteral("Continue (%1 more turns)").arg(team->turnBudget);
        raiseDecision(
            *team, QStringLiteral("budget"),
            QStringLiteral("The team used its %1 automatic turns. Continue?").arg(team->turnBudget),
            QStringLiteral("Automatic turns are paused until you answer. Held messages "
                           "are delivered in order when the team continues."),
            {more, QStringLiteral("Stop")}, more, {}, QStringLiteral("daemon"));
        updates.append({.kind = QStringLiteral("paused"),
                        .text = QStringLiteral("Paused: the team used its %1 automatic turns.")
                                    .arg(team->turnBudget)});
    }
    if (team->paused) {
        team->held.append(heldJson(message));
        if (commit(*team, before, QStringLiteral("daemon"), updates))
            qWarning("Unable to hold the team's %s.", qUtf8Printable(message.kind));
        return;
    }
    ++team->automaticTurns;
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        return;
    if (const QString error = host_->postTeamMessage(message); !error.isEmpty())
        qWarning("Unable to send the team's %s to %s: %s", qUtf8Printable(message.kind),
                 qUtf8Printable(message.toSession), qUtf8Printable(error));
}

void TeamCoordinator::releaseHeld(const qint64 teamId) {
    Team* team = mutableTeam(teamId);
    if (!team || team->paused || team->held.isEmpty())
        return;
    const Team before = *team;
    const QJsonArray held = team->held;
    team->held = {};
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        return;
    for (const auto& value : held) {
        const Team* current = this->team(teamId);
        if (!current)
            return;
        const TeamMessage message = heldMessage(*current, value.toObject());
        // A member that left meanwhile receives nothing of the team's.
        if (!current->member(message.toSession))
            continue;
        // The user's Stop is respected here as well: what only wakes a member to go on by
        // itself is dropped for a member that is stopped now, whenever it was held.
        static const QStringList wakesOnly{QStringLiteral("wait"), QStringLiteral("message_failed"),
                                           QStringLiteral("nudge"), QStringLiteral("stall")};
        if (wakesOnly.contains(message.kind) &&
            stalls_.value(teamId).stopped.contains(message.toSession))
            continue;
        // Past the budget, wake() holds the rest again, still in order.
        wake(*current, message);
    }
}

void TeamCoordinator::scheduleDigest(const qint64 teamId) {
    QTimer*& timer = digestTimers_[teamId];
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, teamId] {
            const Team* current = team(teamId);
            const Member* leader = current ? current->memberWithRole(role::leader) : nullptr;
            const auto conversation =
                leader ? host_->teamConversation(leader->sessionId) : std::nullopt;
            // A busy leader reads the digest when its turn ends (memberTurnEnded).
            if (conversation && conversation->idle)
                sendDigest(teamId);
        });
    }
    timer->start(quietPeriod_);
}

void TeamCoordinator::sendDigest(const qint64 teamId) {
    const Team* team = this->team(teamId);
    const Member* leader = team ? team->memberWithRole(role::leader) : nullptr;
    if (!leader || team->paused)
        return;
    const QList<TeamUpdateRecord> updates = journal_->teamUpdates(teamId, true, 500);
    const bool actionable =
        std::any_of(updates.cbegin(), updates.cend(),
                    [](const TeamUpdateRecord& update) { return update.actionable; });
    if (!actionable)
        return;
    if (QTimer* timer = digestTimers_.value(teamId))
        timer->stop();
    (void)journal_->markTeamUpdatesDigested(teamId, updates.last().id);
    wake(*team, {.teamId = team->id,
                 .teamName = team->name,
                 .toSession = leader->sessionId,
                 .kind = QStringLiteral("digest"),
                 .body = digestText(*team, updates)});
}

void TeamCoordinator::memberTurnEnded(const QString& sessionId,
                                      const std::optional<bool> stoppedByUser) {
    const Team* team = teamOf(sessionId);
    if (!team)
        return;
    const Member* member = team->member(sessionId);
    const QString memberRole = member ? member->role : QString{};
    const qint64 teamId = team->id;
    if (stoppedByUser == true) {
        stalls_[teamId].stopped.insert(sessionId);
        // The user's Stop also ends what the member asked to be woken for: the timer, and
        // a wake that ended while the team was paused and is held for Resume.
        cancelWait(sessionId);
        QJsonArray kept;
        for (const QJsonValue& value : team->held) {
            const QJsonObject message = value.toObject();
            if (message.value(QStringLiteral("kind")).toString() != QStringLiteral("wait") ||
                message.value(QStringLiteral("to")).toString() != sessionId)
                kept.append(value);
        }
        if (Team* changed = kept.size() == team->held.size() ? nullptr : mutableTeam(teamId)) {
            const Team before = *changed;
            changed->held = kept;
            if (commit(*changed, before, QStringLiteral("daemon"), {}))
                qWarning("Unable to drop the held wait of a stopped member.");
        }
    } else if (stoppedByUser == false) {
        stalls_[teamId].stopped.remove(sessionId);
    }
    // The turn is over: a conversation that could not leave a handed-over worktree can now,
    // before the next message to it starts a turn.
    (void)settleRelocations(teamId, sessionId);
    watchForStall(teamId);
    checkMerged(teamId);
    if (memberRole == role::developer)
        checkLanes(teamId, sessionId);
    else if (memberRole == role::leader) {
        askFirstPlanAgain(teamId);
        sendDigest(teamId);
    }
}

} // namespace ketplus::daemon::teams
