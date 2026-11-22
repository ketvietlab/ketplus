#include "teams/TeamContextReset.h"

#include <QStringList>

namespace ketplus::daemon::teams::context_reset {
namespace {

QString line(QString text, const qsizetype bound) {
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return cutText(text.trimmed(), bound);
}

// `text` cut to at most `bytes` of UTF-8 at a character boundary, saying so at its end.
QString withinBytes(const QString& text, const qsizetype bytes) {
    if (text.toUtf8().size() <= bytes)
        return text;
    static const QString tail =
        QStringLiteral("\n(briefing shortened; read the rest with team_get and team_tasks)");
    QString cut = text;
    const qsizetype room = bytes - tail.toUtf8().size();
    while (!cut.isEmpty() && cut.toUtf8().size() > room)
        cut.chop(std::max<qsizetype>(1, (cut.toUtf8().size() - room) / 4));
    if (!cut.isEmpty() && cut.back().isHighSurrogate())
        cut.chop(1);
    return cut + tail;
}

QString memberName(const Team& team, const QString& sessionId) {
    const Member* member = sessionId.isEmpty() ? nullptr : team.member(sessionId);
    return member ? member->name : QStringLiteral("nobody");
}

} // namespace

QString memberTask(const Team& team, const Member& member, const QString& hint) {
    if (member.role == role::leader)
        return {};
    if (!hint.isEmpty() && team.task(hint))
        return hint;
    if (member.role == role::reviewer) {
        const ReviewSlot* slot = team.reviewSlot(member.sessionId);
        return slot ? slot->reviewing : QString{};
    }
    for (const Task& task : team.tasks)
        if (task.owner == member.sessionId && task.state == state::inProgress)
            return task.id;
    return {};
}

QString decide(const Team& team, const Member& member, std::optional<ContextMark>& mark,
               const QString& task, const QString& thread, const qint64 turnSequence) {
    if (team.contextReset == never)
        return {};
    // A context the team has not seen: a member's first, or one a provider handoff began.
    if (!mark || thread.isEmpty() || (!mark->thread.isEmpty() && mark->thread != thread)) {
        mark = ContextMark{.task = task, .thread = thread, .since = turnSequence};
        return {};
    }
    // The fresh thread of the last reset, now that the provider has named it.
    if (mark->thread.isEmpty())
        mark->thread = thread;
    if (team.contextReset == taskAndThreshold && member.role != role::leader && !task.isEmpty()) {
        if (mark->task.isEmpty())
            mark->task = task;
        else if (mark->task != task)
            return taskChanged;
    }
    if (team.contextResetTurns > 0 && turnSequence - mark->since >= team.contextResetTurns)
        return thresholdReached;
    return {};
}

QString briefing(const Team& team, const Member& member, const QString& taskId) {
    QStringList text{QStringLiteral("[KetPlus team briefing] Team \"%1\".").arg(team.name),
                     QStringLiteral("Goal: %1").arg(line(team.goal, 600))};
    QString you = QStringLiteral("You are the %1").arg(member.role);
    if (!member.lane.isEmpty())
        you += QStringLiteral(" of lane \"%1\"").arg(member.lane);
    if (!member.paths.isEmpty())
        you +=
            QStringLiteral(" (paths: %1)").arg(line(member.paths.join(QStringLiteral(", ")), 300));
    text << you + QLatin1Char('.');
    if (const Task* task = taskId.isEmpty() ? nullptr : team.task(taskId)) {
        text << QString{}
             << QStringLiteral("Your task %1 \"%2\": %3, round %4, owner %5.")
                    .arg(task->id, line(task->title, 200), task->state)
                    .arg(task->round)
                    .arg(memberName(team, task->owner));
        if (!task->worktree.isEmpty())
            text << QStringLiteral("Worktree: %1").arg(task->worktree);
        if (!task->criteria.isEmpty()) {
            text << QStringLiteral("Criteria:");
            for (const Criterion& criterion : task->criteria)
                text << QStringLiteral("- [%1] %2")
                            .arg(criterion.met ? QStringLiteral("x") : QStringLiteral(" "),
                                 line(criterion.text, 240));
        }
        const qsizetype first = std::max<qsizetype>(0, task->history.size() - briefingHistory);
        if (first < task->history.size()) {
            text << QStringLiteral("Last history entries:");
            for (qsizetype index = first; index < task->history.size(); ++index) {
                const HistoryEntry& entry = task->history.at(index);
                text << QStringLiteral("- %1 %2 (%3): %4")
                            .arg(entry.at, entry.kind, memberName(team, entry.actor),
                                 line(entry.text, 280));
            }
        }
        text << QString{}
             << QStringLiteral("Read the description and the whole history with team_tasks "
                               "task_id=%1, the team with team_get.")
                    .arg(task->id);
    } else if (member.role == role::leader) {
        QStringList open;
        qsizetype total = 0;
        for (const Task& task : team.tasks) {
            if (task.state == state::done)
                continue;
            if (++total <= briefingOpenTasks)
                open << QStringLiteral("- %1 \"%2\": %3, round %4, owner %5")
                            .arg(task.id, line(task.title, 160), task.state)
                            .arg(task.round)
                            .arg(memberName(team, task.owner));
        }
        text << QString{}
             << (open.isEmpty() ? QStringLiteral("No open tasks.")
                                : QStringLiteral("Open tasks (%1):").arg(total));
        text << open;
        if (total > briefingOpenTasks)
            text << QStringLiteral("- … %1 more").arg(total - briefingOpenTasks);
        text << QString{}
             << QStringLiteral("Read a task with team_tasks task_id=<id>, every open task with "
                               "team_tasks open=true, the team with team_get.");
    } else {
        text << QString{}
             << QStringLiteral("Read your tasks with team_tasks mine=true, the team with "
                               "team_get.");
    }
    return withinBytes(text.join(QLatin1Char('\n')) + QStringLiteral("\n\n"), maximumBriefingBytes);
}

} // namespace ketplus::daemon::teams::context_reset
