#include "teams/TeamContextReset.h"

#include <QRegularExpression>
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
        QStringLiteral("\n(briefing shortened; read the rest with team_get and team_tasks)\n\n");
    QString cut = text;
    const qsizetype room = bytes - tail.toUtf8().size();
    while (!cut.isEmpty() && cut.toUtf8().size() > room)
        cut.chop(std::max<qsizetype>(1, (cut.toUtf8().size() - room) / 4));
    if (!cut.isEmpty() && cut.back().isHighSurrogate())
        cut.chop(1);
    return cut + tail;
}

QStringList readingList(const Task& task) {
    // Inline-code paths in task requirements are explicit hints supplied by the team.
    static const QRegularExpression path(QStringLiteral(
        R"(`((?:[A-Za-z0-9_-][A-Za-z0-9_.-]*/)+[A-Za-z0-9_-][A-Za-z0-9_.-]*|)"
        R"([A-Za-z0-9_-][A-Za-z0-9_.-]*\.(?:md|rst|txt|cpp|cc|cxx|h|hpp|cmake|py|sh|json|)"
        R"(ya?ml|toml|js|ts|tsx|jsx|css|html|qml|swift|rs|go))`)"));
    QStringList sources{task.description};
    for (const Criterion& criterion : task.criteria)
        sources << criterion.text;
    QStringList files{QStringLiteral("AGENTS.md")};
    for (const QString& source : sources) {
        auto matches = path.globalMatch(source);
        while (matches.hasNext()) {
            const QString file = matches.next().captured(1);
            if (file.size() <= 120 && file != QStringLiteral("node.js") && !files.contains(file))
                files << file;
            if (files.size() == 8)
                return files;
        }
    }
    return files;
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

bool cacheWentCold(const Team& team, const ThreadActivity& activity) {
    if (team.contextResetIdleMinutes <= 0 || !activity.lastTurnEnded.isValid() ||
        !activity.now.isValid() || activity.contextTokens < minimumColdContextTokens)
        return false;
    return activity.lastTurnEnded.msecsTo(activity.now) >=
           qint64(team.contextResetIdleMinutes) * 60 * 1000;
}

QString decide(const Team& team, const Member& member, std::optional<ContextMark>& mark,
               const QString& task, const QString& thread, const qint64 turnSequence,
               const ThreadActivity& activity) {
    // The leader works across tasks and is never reset (ADR 0114 §11 as amended): no reason, and
    // no mark is kept for it.
    if (team.contextReset == never || member.role == role::leader)
        return {};
    // A context the team has not seen: a member's first, or one a provider handoff began.
    if (!mark || thread.isEmpty() || (!mark->thread.isEmpty() && mark->thread != thread)) {
        mark = ContextMark{.task = task, .thread = thread, .since = turnSequence};
        return {};
    }
    // The fresh thread of the last reset, now that the provider has named it.
    if (mark->thread.isEmpty())
        mark->thread = thread;
    if (team.contextReset == taskAndThreshold && !task.isEmpty()) {
        if (mark->task.isEmpty())
            mark->task = task;
        else if (mark->task != task)
            return taskChanged;
    }
    if (team.contextResetTurns > 0 && turnSequence - mark->since >= team.contextResetTurns)
        return thresholdReached;
    if (team.contextResetTokens > 0 && activity.contextTokens >= team.contextResetTokens)
        return contextSize;
    if (cacheWentCold(team, activity))
        return cacheCold;
    return {};
}

QString briefing(const Team& team, const Member& member, const QString& taskId) {
    QStringList text{
        QStringLiteral("[KetPlus team briefing] Team \"%1\".").arg(line(team.name, 160)),
        QStringLiteral("Goal: %1").arg(line(team.goal, 600))};
    QString you = QStringLiteral("You are the %1").arg(member.role);
    if (!member.lane.isEmpty())
        you += QStringLiteral(" of lane \"%1\"").arg(line(member.lane, 120));
    text << you + QLatin1Char('.');
    if (const Task* task = taskId.isEmpty() ? nullptr : team.task(taskId)) {
        text << QString{}
             << QStringLiteral("Your task %1 \"%2\": %3, round %4, owner %5.")
                    .arg(task->id, line(task->title, 200), task->state)
                    .arg(task->round)
                    .arg(memberName(team, task->owner));
        if (!task->worktree.isEmpty())
            text << QStringLiteral("Worktree: %1").arg(task->worktree);
        text << QStringLiteral("Files to read in the task worktree:");
        for (const QString& file : readingList(*task))
            text << QStringLiteral("- %1").arg(file);
        if (!task->description.isEmpty())
            text << QStringLiteral("Task summary: %1").arg(line(task->description, 360));
        if (!task->criteria.isEmpty()) {
            text << QStringLiteral("Acceptance criteria (summary):");
            for (const Criterion& criterion : task->criteria)
                text << QStringLiteral("- [%1] %2")
                            .arg(criterion.met ? QStringLiteral("x") : QStringLiteral(" "),
                                 line(criterion.text, 240));
        }
        text << QStringLiteral(
                    "Read the full task with team_tasks task_id=%1, the team with team_get.")
                    .arg(task->id);
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
    } else {
        text << QString{}
             << QStringLiteral("Read your tasks with team_tasks mine=true, the team with "
                               "team_get.");
    }
    return withinBytes(text.join(QLatin1Char('\n')) + QStringLiteral("\n\n"), maximumBriefingBytes);
}

} // namespace ketplus::daemon::teams::context_reset
