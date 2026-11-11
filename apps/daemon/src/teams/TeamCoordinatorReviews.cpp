// ADR 0112 §2: the team's reviewer reviews each task on the engine of ADR 0067 — a `review`
// message with the verdict line — triggered by the task entering In review instead of by a
// finished turn, queued first in first out instead of skipped.
#include "teams/TeamCoordinator.h"

#include <QRegularExpression>

namespace ketplus::daemon::teams {
namespace {

// Below the broker's 32 KiB message limit, leaving room for the criteria and the closing line.
constexpr qsizetype diffBytes = 24 * 1024;
constexpr qsizetype findingsBytes = 6 * 1024;

QString utf8Prefix(const QString& text, const qsizetype maximumBytes) {
    const QByteArray bytes = text.toUtf8();
    if (bytes.size() <= maximumBytes)
        return text;
    QString prefix = QString::fromUtf8(bytes.left(maximumBytes));
    // A cut inside a character decodes to U+FFFD; drop it.
    while (prefix.endsWith(QChar::ReplacementCharacter))
        prefix.chop(1);
    return prefix;
}

} // namespace

QString TeamCoordinator::reviewText(const Team& team, const Task& task,
                                    const std::optional<TaskDiff>& diff) {
    QStringList lines{
        QStringLiteral("Team %1: review task %2 \"%3\" (round %4 of %5).")
            .arg(team.name, task.id, task.title)
            .arg(task.round)
            .arg(team.maxRounds),
        QStringLiteral("Branch: %1, against %2").arg(task.branch, team.integrationBranch),
        QStringLiteral("Worktree: %1").arg(task.worktree)};
    if (!task.description.isEmpty())
        lines << QString{} << task.description;
    lines << QString{} << QStringLiteral("Acceptance criteria:");
    if (task.criteria.isEmpty())
        lines << QStringLiteral("(none written)");
    for (int index = 0; index < task.criteria.size(); ++index)
        lines << QStringLiteral("%1. %2").arg(index + 1).arg(task.criteria.at(index).text);
    lines << QString{};
    if (!diff) {
        lines << QStringLiteral("The diff could not be read; read the branch in the worktree.");
    } else {
        lines << QStringLiteral("Changed files: %1")
                     .arg(diff->files.isEmpty() ? QStringLiteral("none")
                                                : diff->files.join(QStringLiteral(", ")));
        const QString shown = utf8Prefix(diff->diff.trimmed(), diffBytes);
        if (!shown.isEmpty())
            lines << QString{} << QStringLiteral("```diff") << shown << QStringLiteral("```");
        if (shown.size() < diff->diff.trimmed().size())
            lines << QStringLiteral("The diff was shortened to fit one message; read the rest "
                                    "in the worktree.");
    }
    lines << QString{}
          << QStringLiteral("Review read-only against the criteria: say for each one whether it "
                            "is met, then each finding with file, line, severity and a one-line "
                            "summary. End with exactly one line: \"Verdict: findings\" or "
                            "\"Verdict: no findings\".");
    return lines.join(QLatin1Char('\n'));
}

void TeamCoordinator::pumpReviews(const qint64 teamId) {
    Team* team = mutableTeam(teamId);
    if (!team || !team->reviewing.isEmpty() || team->reviewQueue.isEmpty())
        return;
    const Member* reviewer = team->memberWithRole(role::reviewer);
    if (!reviewer)
        return;
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    QString taskId;
    // A task that left In review while it waited is dropped from the queue.
    while (!team->reviewQueue.isEmpty() && taskId.isEmpty()) {
        const QString next = team->reviewQueue.takeFirst();
        if (const Task* task = team->task(next); task && task->state == state::inReview)
            taskId = next;
    }
    if (taskId.isEmpty()) {
        (void)commit(*team, before, QStringLiteral("daemon"), {});
        return;
    }
    Task* task = team->task(taskId);
    ++task->round;
    team->reviewing = taskId;
    const QString reviewerSession = reviewer->sessionId;
    const auto diff = host_->taskDiff(*team, *task);
    addHistory(*task, QStringLiteral("daemon"), QStringLiteral("review"),
               QStringLiteral("Review round %1 of %2 sent to %3.")
                   .arg(task->round)
                   .arg(team->maxRounds)
                   .arg(reviewer->name),
               {{QStringLiteral("round"), task->round}});
    const QString body = reviewText(*team, *task, diff);
    if (commit(*team, before, QStringLiteral("daemon"),
               {{.kind = QStringLiteral("review_requested"),
                 .text = QStringLiteral("%1 is in review (round %2).").arg(taskId).arg(task->round),
                 .taskId = taskId}}))
        return;
    wake(*team, {.teamId = team->id,
                 .teamName = team->name,
                 .toSession = reviewerSession,
                 .kind = QStringLiteral("review"),
                 .taskId = taskId,
                 .body = body,
                 .expectReply = true});
}

void TeamCoordinator::reviewFinished(const QString& reviewerSession, const QString& text,
                                     const bool noFindings, const QString& failure) {
    Team* team = mutableTeamOf(reviewerSession);
    if (!team || team->reviewing.isEmpty())
        return;
    const Member* member = team->member(reviewerSession);
    if (!member || member->role != role::reviewer)
        return;
    const Team before = *team;
    const QString taskId = team->reviewing;
    team->reviewing.clear();
    Task* task = team->task(taskId);
    QList<Update> updates;
    std::optional<TeamMessage> back;
    if (task && task->state == state::inReview) {
        const QString findings = utf8Prefix(text.trimmed(), findingsBytes);
        const int round = task->round;
        if (!failure.isEmpty()) {
            task->resumeState = state::inProgress;
            task->question = QStringLiteral("The review of round %1 did not finish (%2). Hand "
                                            "the task to review again, or hand it over.")
                                 .arg(round)
                                 .arg(failure);
            applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question,
                      updates);
        } else if (noFindings) {
            addHistory(*task, reviewerSession, QStringLiteral("verdict"),
                       QStringLiteral("Round %1: no findings.").arg(round),
                       {{QStringLiteral("verdict"), QStringLiteral("no_findings")},
                        {QStringLiteral("round"), round},
                        {QStringLiteral("text"), findings}});
            applyMove(*team, *task, state::ready, QStringLiteral("daemon"),
                      QStringLiteral("reviewed, no findings"), updates);
            offerMerges(*team, updates);
        } else {
            addHistory(*task, reviewerSession, QStringLiteral("verdict"),
                       QStringLiteral("Round %1: findings.").arg(round),
                       {{QStringLiteral("verdict"), QStringLiteral("findings")},
                        {QStringLiteral("round"), round},
                        {QStringLiteral("text"), findings}});
            if (round >= team->maxRounds) {
                // Past the limit the leader decides (ADR 0112 §2).
                task->resumeState = state::inProgress;
                task->question = QStringLiteral("Review round limit reached (%1 of %2) with "
                                                "findings.")
                                     .arg(round)
                                     .arg(team->maxRounds);
                applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question,
                          updates);
                updates.append({.kind = QStringLiteral("round_limit"),
                                .text = QStringLiteral("%1 still has findings after %2 rounds.")
                                            .arg(taskId)
                                            .arg(round),
                                .taskId = taskId,
                                .actionable = true});
            } else {
                applyMove(*team, *task, state::inProgress, QStringLiteral("daemon"),
                          QStringLiteral("review round %1 found issues").arg(round), updates);
                static const QRegularExpression verdictLine(
                    QStringLiteral("^\\W*verdict\\W*:"), QRegularExpression::CaseInsensitiveOption |
                                                             QRegularExpression::MultilineOption);
                const bool noVerdict = !verdictLine.match(text).hasMatch();
                back = TeamMessage{
                    .teamId = team->id,
                    .teamName = team->name,
                    .toSession = task->owner,
                    .kind = QStringLiteral("review"),
                    .taskId = taskId,
                    .body =
                        QStringList{
                            QStringLiteral("Team %1: the review of %2 \"%3\" (round %4 of %5) "
                                           "found issues.")
                                .arg(team->name, taskId, task->title)
                                .arg(round)
                                .arg(team->maxRounds),
                            noVerdict ? QStringLiteral("(The reply had no verdict line, so it "
                                                       "counts as findings.)")
                                      : QString{},
                            QString{}, findings, QString{},
                            QStringLiteral("Fix them, commit, and move the task to in_review again "
                                           "with team_task_update.")}
                            .join(QLatin1Char('\n'))};
            }
        }
    }
    if (commit(*team, before, QStringLiteral("daemon"), updates))
        return;
    if (back)
        wake(*team, *back);
}

} // namespace ketplus::daemon::teams
