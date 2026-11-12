// ADR 0114 §2: a reviewer of the team reviews each task on the engine of ADR 0067 — a `review`
// message with the verdict line — triggered by the task entering In review instead of by a
// finished turn, queued first in first out instead of skipped.
#include "teams/TeamCoordinator.h"

#include "agents/ReviewVerdict.h"

#include <QDir>
#include <QJsonObject>
#include <QPointer>
#include <QUuid>

#include <algorithm>

namespace ketplus::daemon::teams {
namespace {

// Below the broker's 32 KiB message limit, leaving room for the criteria and the closing line.
constexpr qsizetype diffBytes = 24 * 1024;
constexpr qsizetype findingsBytes = 6 * 1024;
// Below GitHub's 65,536 characters for one comment.
constexpr qsizetype commentBytes = 60 * 1024;

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

// The first queued task that is still In review: the one the reviewer's next review is for.
const Task* nextInReview(const Team& team, const ReviewSlot& slot) {
    for (const QString& id : slot.queue)
        if (const Task* task = team.task(id); task && task->state == state::inReview)
            return task;
    return nullptr;
}

QString diffKey(const qint64 teamId, const QString& reviewer) {
    return QStringLiteral("%1/%2").arg(teamId).arg(reviewer);
}

bool onPullRequest(const Task& task) { return task.reviewPlace == place::pullRequest; }

// The "of n" a round shows: a task past a limit lowered after its last round shows its own
// round as the last one, not "round 2 of 1".
int shownLimit(const int limit, const int round) { return std::max(limit, round); }

// T-40: a comment on a public pull request names no folder of this computer. The task's
// worktree, the team's repository and the home folder are replaced, longest first.
QString publicText(QString text, const Team& team, const Task& task) {
    QList<std::pair<QString, QString>> paths{{task.worktree, QStringLiteral("<task worktree>")},
                                             {team.repository, QStringLiteral("<repository>")},
                                             {QDir::homePath(), QStringLiteral("~")}};
    std::sort(paths.begin(), paths.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [path, name] : paths) {
        const QString folder = QDir::cleanPath(path);
        if (folder.size() > 1)
            text.replace(folder, name);
    }
    return text;
}

} // namespace

int TeamCoordinator::roundLimit(const Team& team, const Task& task) {
    return task.roundLimit > 0 ? task.roundLimit : team.maxRounds;
}

QString TeamCoordinator::noVerdictQuestion(const int round, const int replies) {
    return QStringLiteral("The review of round %1 has no verdict after %2 replies. Accept it "
                          "with team_review_accept, or hand the task to review again.")
        .arg(round)
        .arg(replies);
}

QString TeamCoordinator::roundLimitQuestion(const QString& language, const int round,
                                            const int maxRounds) {
    return say(language,
               QT_TRANSLATE_NOOP("Team", "Review round limit reached (%1 of %2) with findings."))
        .arg(round)
        .arg(maxRounds);
}

QString TeamCoordinator::reviewText(const Team& team, const Task& task,
                                    const std::optional<TaskDiff>& diff) {
    QStringList lines{
        QStringLiteral("Team %1: review task %2 \"%3\" (round %4 of %5).")
            .arg(team.name, task.id, task.title)
            .arg(task.round)
            .arg(shownLimit(roundLimit(team, task), task.round)),
        QStringLiteral("Branch: %1, against %2").arg(task.branch, team.integrationBranch),
        QStringLiteral("Worktree: %1").arg(task.worktree)};
    if (TeamCoordinator::recordsPullRequest(task))
        lines << QStringLiteral("Pull request: %1").arg(task.pullRequest);
    if (!task.headCommit.isEmpty())
        lines << QStringLiteral("Head commit: %1").arg(task.headCommit);
    if (!task.description.isEmpty())
        lines << QString{} << task.description;
    lines << QString{} << QStringLiteral("Acceptance criteria:");
    if (task.criteria.isEmpty())
        lines << QStringLiteral("(none written)");
    for (int index = 0; index < task.criteria.size(); ++index)
        lines << QStringLiteral("%1. %2").arg(index + 1).arg(task.criteria.at(index).text);
    lines << QString{};
    if (diff && diff->outsideBranch) {
        // T-38: work done in another worktree or repository, named by the hand to review.
        lines << (task.headCommit.isEmpty()
                      ? QStringLiteral("The branch has no commit of its own: review the pull "
                                       "request above.")
                      : QStringLiteral("The branch has no commit of its own: review the pull "
                                       "request above at its head commit."));
    } else if (!diff) {
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
    if (task.missingVerdicts > 0)
        lines << QString{}
              << QStringLiteral("Your last reply to this round had no verdict line, so it was "
                                "not counted. Review again and end with the verdict line.");
    lines << QString{}
          << QStringLiteral("Review read-only against the criteria: say for each one whether it "
                            "is met, then each finding with file, line, severity and a one-line "
                            "summary. End with exactly one line: \"Verdict: findings\" or "
                            "\"Verdict: no findings\".");
    // T-40: the place of this round, chosen when the task entered review.
    if (onPullRequest(task))
        lines << QString{}
              << QStringLiteral("This team reviews on the pull request: your reply, as you write "
                                "it, is posted as a review comment on %1, and the board keeps "
                                "only the verdict and the comment's link. Do not post it "
                                "yourself; the review stays read-only. The comment may be "
                                "public: write it in English, cite files by their path in the "
                                "repository, and name no folder of this computer and no "
                                "person.")
                     .arg(task.pullRequest);
    else if (team.reviewPlace == place::pullRequest)
        lines << QString{}
              << QStringLiteral("This team reviews on the pull request, but this task has no pull "
                                "request recorded, so this round is reviewed on the board: your "
                                "reply is the record.");
    return lines.join(QLatin1Char('\n'));
}

void TeamCoordinator::pumpReviews(const qint64 teamId) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    // T-23: each idle reviewer takes the next task of its own queue.
    QStringList idle;
    for (const ReviewSlot& slot : std::as_const(team->reviews))
        if (slot.reviewing.isEmpty() && !slot.queue.isEmpty() &&
            !reviewDiffs_.contains(diffKey(teamId, slot.reviewer)))
            idle.append(slot.reviewer);
    for (const QString& reviewer : std::as_const(idle)) {
        team = mutableTeam(teamId);
        const ReviewSlot* slot = team ? team->reviewSlot(reviewer) : nullptr;
        // A commit() below pumps again and may already have asked for this reviewer's diff.
        if (!slot || !team->member(reviewer) || !slot->reviewing.isEmpty() ||
            reviewDiffs_.contains(diffKey(teamId, reviewer)))
            continue;
        const Task* next = nextInReview(std::as_const(*team), *slot);
        if (!next) {
            // Every queued task left In review while it waited: the queue is emptied.
            const Team before = *team;
            team->reviewSlot(reviewer)->queue.clear();
            (void)commit(*team, before, QStringLiteral("daemon"), {});
            continue;
        }
        reviewDiffs_.insert(diffKey(teamId, reviewer));
        host_->askDiff(
            *team, *next,
            [self = QPointer<TeamCoordinator>(this), teamId, reviewer, asked = *next,
             repository = team->repository,
             integration = team->integrationBranch](const std::optional<TaskDiff>& diff) {
                if (!self)
                    return;
                self->reviewDiffs_.remove(diffKey(teamId, reviewer));
                self->startReview(teamId, reviewer, asked, repository, integration, diff);
            });
    }
}

void TeamCoordinator::startReview(const qint64 teamId, const QString& reviewerSession,
                                  const Task& asked, const QString& repository,
                                  const QString& integration, const std::optional<TaskDiff>& diff) {
    Team* team = mutableTeam(teamId);
    const ReviewSlot* slot = team ? team->reviewSlot(reviewerSession) : nullptr;
    const Member* reviewer = team ? team->member(reviewerSession) : nullptr;
    // The reviewer left, or took another review, while its diff was read.
    if (!slot || !reviewer || !slot->reviewing.isEmpty() || slot->queue.isEmpty()) {
        if (team)
            pumpReviews(teamId);
        return;
    }
    // The diff is of one task's branch against the integration branch the team had. Another
    // task at the head of the queue, or another branch on either side, needs its own diff.
    const Task* next = nextInReview(std::as_const(*team), *slot);
    if (!next || next->id != asked.id || next->branch != asked.branch ||
        next->worktree != asked.worktree || next->baseCommit != asked.baseCommit ||
        next->headCommit != asked.headCommit || team->repository != repository ||
        team->integrationBranch != integration) {
        pumpReviews(teamId);
        return;
    }
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    const QString taskId = asked.id;
    const QString reviewerName = reviewer->name;
    ReviewSlot* taking = team->reviewSlot(reviewerSession);
    // The tasks queued before it left In review while they waited.
    while (!taking->queue.isEmpty() && taking->queue.takeFirst() != taskId) {
    }
    Task* task = team->task(taskId);
    ++task->round;
    task->reviewer = reviewerSession;
    // T-40: the team's settings as the task enters this round; a change applies afterwards.
    task->roundLimit = team->maxRounds;
    task->reviewPlace = team->reviewPlace == place::pullRequest && recordsPullRequest(*task)
                            ? place::pullRequest
                            : place::board;
    const bool noPullRequest = team->reviewPlace == place::pullRequest && !onPullRequest(*task);
    task->reviewComment.clear();
    taking->reviewing = taskId;
    // Bound before it is sent: a review held while the team is paused, or one whose
    // message is still on its way, cannot be answered by another request.
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    taking->message = requestId;
    QString sent = (task->missingVerdicts > 0
                        ? QStringLiteral("Review round %1 of %2 sent to %3 again: the last reply "
                                         "had no verdict.")
                        : QStringLiteral("Review round %1 of %2 sent to %3."))
                       .arg(task->round)
                       .arg(shownLimit(task->roundLimit, task->round))
                       .arg(reviewerName);
    if (onPullRequest(*task))
        sent += QStringLiteral(" Its findings go to the pull request.");
    else if (noPullRequest)
        sent += QStringLiteral(" Reviewed on the board: the task has no pull request.");
    addHistory(*task, QStringLiteral("daemon"), QStringLiteral("review"), sent,
               {{QStringLiteral("round"), task->round},
                {QStringLiteral("reviewer"), reviewerName},
                {QStringLiteral("place"), task->reviewPlace}});
    const QString body = reviewText(*team, *task, diff);
    if (commit(
            *team, before, QStringLiteral("daemon"),
            {{.kind = QStringLiteral("review_requested"),
              .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 is in review (round %2)."))
                          .arg(taskId)
                          .arg(task->round),
              .taskId = taskId}}))
        return;
    wake(*team, {.teamId = team->id,
                 .teamName = team->name,
                 .toSession = reviewerSession,
                 .kind = QStringLiteral("review"),
                 .taskId = taskId,
                 .body = body,
                 .expectReply = true,
                 .id = requestId});
}

void TeamCoordinator::reviewFinished(const QString& reviewerSession, const QString& messageId,
                                     const QString& text, const bool noFindings,
                                     const QString& failure) {
    // T-40: a verdict of a round on the pull request is posted there first, with the review
    // still bound, so a stale answer meanwhile is ignored as before.
    const Team* team = teamOf(reviewerSession);
    const ReviewSlot* slot = team ? team->reviewSlot(reviewerSession) : nullptr;
    const Task* task = slot && !slot->reviewing.isEmpty() ? team->task(slot->reviewing) : nullptr;
    const bool verdict = noFindings || reviewVerdict(text) != ReviewVerdict::none;
    if (!task || task->state != state::inReview || !onPullRequest(*task) || !failure.isEmpty() ||
        !verdict || slot->message.isEmpty() || messageId != slot->message) {
        recordReview(reviewerSession, messageId, text, noFindings, failure, std::nullopt);
        return;
    }
    if (reviewComments_.contains(messageId))
        return;
    reviewComments_.insert(messageId);
    // No member's name in it: the comment may be on a public repository.
    const QString body =
        QStringList{QStringLiteral("**Team review of %1, round %2 of %3**")
                        .arg(task->id)
                        .arg(task->round)
                        .arg(shownLimit(roundLimit(*team, *task), task->round)),
                    QString{}, utf8Prefix(publicText(text.trimmed(), *team, *task), commentBytes)}
            .join(QLatin1Char('\n'));
    host_->askPostReviewComment(*team, *task, body,
                                [self = QPointer<TeamCoordinator>(this), reviewerSession, messageId,
                                 text, noFindings, failure](const ReviewComment& comment) {
                                    if (!self)
                                        return;
                                    self->reviewComments_.remove(messageId);
                                    self->recordReview(reviewerSession, messageId, text, noFindings,
                                                       failure, comment);
                                });
}

void TeamCoordinator::recordReview(const QString& reviewerSession, const QString& messageId,
                                   const QString& text, const bool noFindings,
                                   const QString& failure,
                                   const std::optional<ReviewComment>& comment) {
    Team* team = mutableTeamOf(reviewerSession);
    const ReviewSlot* slot = team ? team->reviewSlot(reviewerSession) : nullptr;
    if (!slot || slot->reviewing.isEmpty())
        return;
    // Only the request this review was bound to answers it; an earlier or unrelated request
    // that ends now does not. Compatibility, and nothing else: a review that was in flight
    // in a journal written before reviews were bound has no id, and is matched as it was
    // then, by the reviewer and the task. No review started by this version is in that
    // state, held or sent.
    const bool unbound = slot->message.isEmpty();
    if (!unbound && messageId != slot->message)
        return;
    const Member* member = team->member(reviewerSession);
    if (!member || member->role != role::reviewer)
        return;
    const Team before = *team;
    ReviewSlot* answered = team->reviewSlot(reviewerSession);
    const QString taskId = answered->reviewing;
    answered->reviewing.clear();
    answered->message.clear();
    Task* task = team->task(taskId);
    QList<Update> updates;
    std::optional<TeamMessage> back;
    bool askAgain = false;
    // T-19: a reply without a verdict line is neither findings nor no findings.
    const bool noVerdict = !noFindings && reviewVerdict(text) == ReviewVerdict::none;
    if (task && task->state == state::inReview) {
        const QString findings = utf8Prefix(text.trimmed(), findingsBytes);
        const int round = task->round;
        const int limit = roundLimit(*team, *task);
        // T-40: on the pull request the board keeps the verdict and the comment's link; a
        // comment that could not be posted leaves the findings on the board, with why.
        const QString link = comment ? comment->url : QString{};
        if (comment)
            task->reviewComment = link;
        const auto verdictData = [&](const QString& verdict) {
            QJsonObject data{
                {QStringLiteral("verdict"), verdict},
                {QStringLiteral("round"), round},
                {QStringLiteral("place"), link.isEmpty() ? place::board : place::pullRequest}};
            if (!link.isEmpty())
                data.insert(QStringLiteral("comment_url"), link);
            else
                data.insert(QStringLiteral("text"), findings);
            if (comment && link.isEmpty())
                data.insert(QStringLiteral("comment_error"), comment->error);
            return data;
        };
        const auto where = [&] {
            if (!comment)
                return QString{};
            return link.isEmpty()
                       ? QStringLiteral(" The comment could not be posted on the pull request "
                                        "(%1), so the findings are kept on the board.")
                             .arg(comment->error)
                       : QStringLiteral(" On the pull request: %1").arg(link);
        };
        if (failure.isEmpty() && !noVerdict)
            task->missingVerdicts = 0;
        if (!failure.isEmpty()) {
            task->resumeState = state::inProgress;
            task->question =
                say(team->language,
                    QT_TRANSLATE_NOOP("Team", "The review of round %1 did not finish (%2). Hand "
                                              "the task to review again, or hand it over."))
                    .arg(round)
                    .arg(failure);
            applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question,
                      updates);
        } else if (noVerdict) {
            // Not counted: the round is asked again, as the same round, a bounded number of
            // times; then the leader is asked once.
            --task->round;
            ++task->missingVerdicts;
            askAgain = task->missingVerdicts <= verdictRetries;
            addHistory(
                *task, reviewerSession, QStringLiteral("verdict"),
                (askAgain
                     ? say(team->language,
                           QT_TRANSLATE_NOOP("Team",
                                             "Round %1: no verdict. The reply had no verdict "
                                             "line; the round is not counted and the "
                                             "reviewer is asked again."))
                     : say(team->language,
                           QT_TRANSLATE_NOOP("Team", "Round %1: no verdict again. The round is not "
                                                     "counted; the leader decides.")))
                    .arg(round),
                {{QStringLiteral("verdict"), verdictName(ReviewVerdict::none)},
                 {QStringLiteral("round"), round},
                 {QStringLiteral("text"), findings}});
            if (askAgain) {
                // The same reviewer is asked again, first.
                answered->queue.removeAll(taskId);
                answered->queue.prepend(taskId);
            } else {
                // One line for the leader: the blocked task says why.
                task->resumeState = state::inProgress;
                task->question = noVerdictQuestion(round, task->missingVerdicts);
                applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question,
                          updates);
            }
        } else if (noFindings) {
            addHistory(*task, reviewerSession, QStringLiteral("verdict"),
                       say(team->language, QT_TRANSLATE_NOOP("Team", "Round %1: no findings."))
                               .arg(round) +
                           where(),
                       verdictData(QStringLiteral("no_findings")));
            applyMove(*team, *task, state::ready, QStringLiteral("daemon"),
                      say(team->language, QT_TRANSLATE_NOOP("Team", "reviewed, no findings")),
                      updates);
            offerMerges(*team, updates);
        } else {
            addHistory(
                *task, reviewerSession, QStringLiteral("verdict"),
                say(team->language, QT_TRANSLATE_NOOP("Team", "Round %1: findings.")).arg(round) +
                    where(),
                verdictData(QStringLiteral("findings")));
            if (round >= limit) {
                // Past the limit the leader decides (ADR 0114 §2); with one round, after it.
                task->resumeState = state::inProgress;
                task->question =
                    roundLimitQuestion(team->language, round, shownLimit(limit, round));
                if (!link.isEmpty())
                    task->question +=
                        say(team->language, QT_TRANSLATE_NOOP("Team", " The findings: %1"))
                            .arg(link);
                applyMove(*team, *task, state::blocked, QStringLiteral("daemon"), task->question,
                          updates);
                updates.append({.kind = QStringLiteral("round_limit"),
                                .text = say(team->language,
                                            QT_TRANSLATE_NOOP(
                                                "Team", "%1 still has findings after %2 rounds."))
                                            .arg(taskId)
                                            .arg(round),
                                .taskId = taskId,
                                .actionable = true});
            } else {
                applyMove(
                    *team, *task, state::inProgress, QStringLiteral("daemon"),
                    say(team->language, QT_TRANSLATE_NOOP("Team", "review round %1 found issues"))
                        .arg(round),
                    updates);
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
                                .arg(shownLimit(limit, round)),
                            QString{},
                            link.isEmpty()
                                ? findings
                                : QStringLiteral("The findings are in the review comment %1:\n\n%2")
                                      .arg(link, findings),
                            QString{},
                            QStringLiteral("Fix them, commit, and move the task to in_review again "
                                           "with team_task_update.")}
                            .join(QLatin1Char('\n'))};
            }
        }
    }
    if (commit(*team, before, QStringLiteral("daemon"), updates))
        return;
    // commit() sent the next review of this reviewer, or the same one asked again.
    if (back)
        wake(*team, *back);
}

} // namespace ketplus::daemon::teams
