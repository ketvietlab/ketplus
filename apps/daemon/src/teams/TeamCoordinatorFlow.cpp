// ADR 0114 §7 and open point 2: what starts a member's turn, the budget of automatic turns,
// the messages held while a team is paused, the outbox (T-28) and the leader's digest.
#include "storage/LocalJournal.h"
#include "teams/TeamCoordinator.h"

#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace ketplus::daemon::teams {
namespace {

QJsonObject heldJson(const TeamMessage& message) {
    return {{QStringLiteral("to"), message.toSession},
            {QStringLiteral("from"), message.fromSession},
            {QStringLiteral("kind"), message.kind},
            {QStringLiteral("task_id"), message.taskId},
            {QStringLiteral("body"), message.body},
            {QStringLiteral("expect_reply"), message.expectReply},
            {QStringLiteral("id"), message.id},
            {QStringLiteral("point_at"), message.pointAt}};
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
            .id = json.value(QStringLiteral("id")).toString(),
            .pointAt = json.value(QStringLiteral("point_at")).toString()};
}

// Every update since the last digest, grouped by task, the actionable ones marked.
QString digestText(const Team& team, const QList<TeamUpdateRecord>& updates) {
    // T-39: a member's wait replaces its earlier one, so only the latest wait of each member is
    // listed; the earlier ones would only repeat it.
    QHash<QString, qint64> latestWait;
    for (const TeamUpdateRecord& update : updates)
        if (update.kind == QStringLiteral("wait"))
            latestWait.insert(update.actor, update.id);
    QStringList order;
    QHash<QString, QStringList> lines;
    for (const TeamUpdateRecord& update : updates) {
        if (update.kind == QStringLiteral("wait") && latestWait.value(update.actor) != update.id)
            continue;
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
    // A turn the team starts never runs in a worktree its member was handed out of. With the
    // outbox, that is settled when the message is sent; without it (the coordinator's own
    // tests), the host switches at once and this is settled before the lines below.
    if (!deferSending_)
        settleRelocations(teamId, message.toSession, {});
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    const Team before = *team;
    QList<Update> updates;
    pauseIfBudgetUsed(*team, updates);
    if (team->paused) {
        team->held.append(heldJson(message));
        if (commit(*team, before, QStringLiteral("daemon"), updates))
            qWarning("Unable to hold the team's %s.", qUtf8Printable(message.kind));
        return;
    }
    ++team->automaticTurns;
    if (!deferSending_) {
        if (commit(*team, before, QStringLiteral("daemon"), {}))
            return;
        send(teamId, message);
        return;
    }
    // T-28: stored with the turn it counts, sent once the command is answered. The id is
    // chosen now, so a send the daemon did not get to forget is not sent again.
    TeamMessage queued = message;
    if (queued.id.isEmpty())
        queued.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    team->outbox.append(heldJson(queued));
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        return;
    scheduleOutbox();
}

void TeamCoordinator::pauseIfBudgetUsed(Team& team, QList<Update>& updates) {
    if (team.paused || team.automaticTurns < team.turnBudget)
        return;
    // The budget is used up: pause once and ask (ADR 0114, open point 2).
    team.paused = true;
    team.pausedReason = QStringLiteral("budget");
    const QString more = QStringLiteral("Continue (%1 more turns)").arg(team.turnBudget);
    raiseDecision(team, QStringLiteral("budget"),
                  QStringLiteral("The team used its %1 automatic turns. Continue?")
                      .arg(team.turnBudget),
                  QStringLiteral("Automatic turns are paused until you answer. Held messages "
                                 "are delivered in order when the team continues."),
                  {more, QStringLiteral("Stop")}, more, {}, QStringLiteral("daemon"));
    updates.append({.kind = QStringLiteral("paused"),
                    .text = QStringLiteral("Paused: the team used its %1 automatic turns.")
                                .arg(team.turnBudget)});
}

QString TeamCoordinator::takeMemberTurn(const QString& sessionId) {
    Team* team = mutableTeamOf(sessionId);
    if (!team)
        return {};
    const Team before = *team;
    QList<Update> updates;
    pauseIfBudgetUsed(*team, updates);
    if (team->paused) {
        if (!updates.isEmpty() && commit(*team, before, QStringLiteral("daemon"), updates))
            qWarning("Unable to pause team %lld for its budget.", static_cast<long long>(before.id));
        return team->pausedReason == QStringLiteral("budget") ? QStringLiteral("team_budget")
                                                              : QStringLiteral("team_paused");
    }
    ++team->automaticTurns;
    // A journal that cannot count the turn does not stop the team's work: the turn runs.
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        qWarning("Unable to count an automatic turn of team %lld.",
                 static_cast<long long>(before.id));
    return {};
}

void TeamCoordinator::send(const qint64 teamId, const TeamMessage& message) {
    if (!message.pointAt.isEmpty())
        notePointError(teamId, message,
                       host_->pointConversationAt(message.toSession, message.pointAt));
    if (const QString error = host_->postTeamMessage(message); !error.isEmpty())
        qWarning("Unable to send the team's %s to %s: %s", qUtf8Printable(message.kind),
                 qUtf8Printable(message.toSession), qUtf8Printable(error));
}

void TeamCoordinator::forgetRelocations(const qint64 teamId, const QString& sessionId) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    const Team before = *team;
    const qsizetype removed = team->relocations.removeIf(
        [&sessionId](const Relocation& move) { return move.session == sessionId; });
    if (removed > 0 && commit(*team, before, QStringLiteral("daemon"), {}))
        qWarning("Unable to forget a settled move out of a handed-over worktree.");
}

void TeamCoordinator::notePointError(const qint64 teamId, const TeamMessage& message,
                                     const QString& error) {
    if (error.isEmpty())
        return;
    Team* stored = mutableTeam(teamId);
    if (!stored || !stored->task(message.taskId))
        return;
    const Team before = *stored;
    addHistory(*stored->task(message.taskId), QStringLiteral("daemon"), QStringLiteral("worktree"),
               QStringLiteral("The owner's conversation was not moved to %1: %2")
                   .arg(message.pointAt, error));
    (void)commit(*stored, before, QStringLiteral("daemon"), {});
}

void TeamCoordinator::scheduleOutbox() {
    if (outboxScheduled_)
        return;
    outboxScheduled_ = true;
    QTimer::singleShot(0, this, &TeamCoordinator::sendFromOutbox);
}

// One entry per pass, oldest first, so the events of running conversations and the next
// command are not held up behind a long outbox. Sent first, then forgotten: a stop between
// the two finds the message queued and only forgets it.
void TeamCoordinator::sendFromOutbox() {
    outboxScheduled_ = false;
    if (outboxSending_)
        return;
    QList<qint64> ids = teams_.keys();
    std::sort(ids.begin(), ids.end());
    for (const qint64 id : std::as_const(ids)) {
        const Team* team = this->team(id);
        if (!team || team->outbox.isEmpty())
            continue;
        const QJsonObject entry = team->outbox.first().toObject();
        const TeamMessage message = heldMessage(*team, entry);
        // A member that left meanwhile receives nothing of the team's.
        if (!stillDelivers(id, message)) {
            finishOutboxEntry(id, entry, false);
            return;
        }
        // Git runs off this thread; the entry stays stored meanwhile, and the next entry
        // waits for it. T-32: without a worktree of its own, the message waits until its
        // conversation has left a worktree it was handed out of.
        outboxSending_ = true;
        const QPointer<TeamCoordinator> self(this);
        const auto finish = [self, id, entry, message](const QString&) {
            if (!self)
                return;
            self->outboxSending_ = false;
            self->finishOutboxEntry(id, entry, self->stillDelivers(id, message));
        };
        if (message.pointAt.isEmpty()) {
            settleRelocations(id, message.toSession, finish);
            return;
        }
        // The switch to the task's worktree settles a pending move of the same conversation
        // once it worked; when it failed, the move is settled before the message is sent.
        whenConversationFree(message.toSession, [self, id, message, finish] {
            if (!self)
                return;
            self->host_->askPointConversationAt(
                message.toSession, message.pointAt,
                [self, id, message, finish](const QString& error) {
                    if (!self)
                        return;
                    self->notePointError(id, message, error);
                    if (error.isEmpty()) {
                        self->forgetRelocations(id, message.toSession);
                        finish({});
                        self->conversationFree(message.toSession);
                        return;
                    }
                    self->settleStep(id, message.toSession,
                                     [self, message, finish](const QString& waiting) {
                                         finish(waiting);
                                         if (self)
                                             self->conversationFree(message.toSession);
                                     });
                });
        });
        return;
    }
}

bool TeamCoordinator::stillDelivers(const qint64 teamId, const TeamMessage& message) const {
    const Team* current = team(teamId);
    return current && current->member(message.toSession) && !host_->teamMessagePosted(message.id);
}

// Sent first, then forgotten: a stop between the two finds the message queued and only
// forgets it.
void TeamCoordinator::finishOutboxEntry(const qint64 teamId, const QJsonObject& entry,
                                        const bool deliver) {
    if (const Team* team = this->team(teamId); team && deliver) {
        const TeamMessage message = heldMessage(*team, entry);
        if (const QString error = host_->postTeamMessage(message); !error.isEmpty())
            qWarning("Unable to send the team's %s to %s: %s", qUtf8Printable(message.kind),
                     qUtf8Printable(message.toSession), qUtf8Printable(error));
    }
    Team* current = mutableTeam(teamId);
    if (current) {
        const Team before = *current;
        for (qsizetype index = 0; index < current->outbox.size(); ++index)
            if (current->outbox.at(index).toObject().value(QStringLiteral("id")) ==
                entry.value(QStringLiteral("id"))) {
                current->outbox.removeAt(index);
                break;
            }
        // A journal that refuses keeps the entry; the next start sends what is left.
        if (commit(*current, before, QStringLiteral("daemon"), {})) {
            qWarning("Unable to forget a sent team message.");
            return;
        }
    }
    for (const qint64 id : teams_.keys())
        if (const Team* team = this->team(id); team && !team->outbox.isEmpty()) {
            scheduleOutbox();
            return;
        }
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
    const QList<TeamUpdateRecord> updates = updateRecords(teamId, true, 500);
    // A wait is shown, but it is never the reason for a digest (T-39): a digest that would only
    // repeat waits, the leader's own or a developer's, is not sent and starts no turn. The
    // waits stay undigested and ride along with the next digest that has something to act on.
    const bool actionable =
        std::any_of(updates.cbegin(), updates.cend(), [](const TeamUpdateRecord& update) {
            return update.actionable && update.kind != QStringLiteral("wait");
        });
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
    // before the next message to it starts a turn (the outbox waits for this switch).
    settleRelocations(teamId, sessionId, {});
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
