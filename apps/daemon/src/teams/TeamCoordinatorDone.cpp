// ADR 0118: a task is done when its pull request is merged on the remote, or when the leader
// closes it by hand. Git's own rule (TeamCoordinatorMerges.cpp) stays for tasks merged locally.
#include "teams/TeamCoordinator.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace ketplus::daemon::teams {
namespace {

const QString pullRequestKind = QStringLiteral("pull_request");

bool isPullRequestUrl(const QString& text) {
    static const QRegularExpression url(QStringLiteral("^https?://\\S+$"));
    return url.match(text.trimmed()).hasMatch();
}

bool isCommit(const QString& text) {
    static const QRegularExpression commit(QStringLiteral("^[0-9a-fA-F]{7,40}$"));
    return commit.match(text.trimmed()).hasMatch();
}

// The last thing the board said about the task's pull request, so the same reason is said once.
QString lastPullRequestNote(const Task& task) {
    for (auto it = task.history.crbegin(); it != task.history.crend(); ++it)
        if (it->kind == pullRequestKind)
            return it->text;
    return {};
}

} // namespace

void TeamCoordinator::setPullRequestPeriod(const int periodMs, const int minimumGapMs) {
    pullRequestPeriod_ = periodMs;
    pullRequestGap_ = minimumGapMs;
    pullRequestCheckedAt_.clear();
    if (pullRequestTimer_)
        pullRequestTimer_->setInterval(periodMs);
}

void TeamCoordinator::checkPullRequests(const qint64 teamId) {
    if (!pullRequestTimer_) {
        // One timer for every team: a merge made while nobody reads the team is still seen.
        pullRequestTimer_ = new QTimer(this);
        pullRequestTimer_->setInterval(pullRequestPeriod_);
        connect(pullRequestTimer_, &QTimer::timeout, this, [this] {
            const QList<qint64> ids = teams_.keys();
            for (const qint64 id : ids)
                checkPullRequests(id);
        });
        pullRequestTimer_->start();
    }
    const Team* team = this->team(teamId);
    if (!team || pullRequestChecks_.contains(teamId))
        return;
    QStringList urls;
    for (const Task& task : team->tasks)
        if (task.state != state::done && isPullRequestUrl(task.pullRequest) &&
            !urls.contains(task.pullRequest.trimmed()))
            urls.append(task.pullRequest.trimmed());
    if (urls.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto last = pullRequestCheckedAt_.constFind(teamId);
    if (last != pullRequestCheckedAt_.cend() && now - *last < pullRequestGap_)
        return;
    pullRequestCheckedAt_.insert(teamId, now);
    // Bounded: one request per pull request, at most a page of them per check, the next page
    // the next time, so a large board is gone through in turn.
    std::sort(urls.begin(), urls.end());
    if (urls.size() > maxPullRequestsPerCheck) {
        const int start = pullRequestCursor_.value(teamId) % urls.size();
        QStringList page;
        for (int i = 0; i < maxPullRequestsPerCheck; ++i)
            page.append(urls.at((start + i) % urls.size()));
        pullRequestCursor_.insert(teamId, start + maxPullRequestsPerCheck);
        urls = page;
    }
    pullRequestChecks_.insert(teamId);
    host_->askPullRequests(*team, urls,
                           [self = QPointer<TeamCoordinator>(this),
                            teamId](const QHash<QString, PullRequestFact>& facts) {
                               if (!self)
                                   return;
                               self->pullRequestChecks_.remove(teamId);
                               self->applyPullRequestFacts(teamId, facts);
                           });
}

void TeamCoordinator::applyPullRequestFacts(const qint64 teamId,
                                            const QHash<QString, PullRequestFact>& facts) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    QList<Update> updates;
    bool finished = false;
    for (Task& task : team->tasks) {
        const auto fact = facts.constFind(task.pullRequest.trimmed());
        if (task.state == state::done || fact == facts.cend())
            continue;
        if (fact->state == QStringLiteral("merged")) {
            const QString commit = fact->mergeCommit;
            finishTask(
                *team, task, QStringLiteral("daemon"),
                commit.isEmpty()
                    ? QStringLiteral("%1 is merged").arg(task.pullRequest)
                    : QStringLiteral("%1 is merged as %2").arg(task.pullRequest, commit.left(12)),
                QStringLiteral("merged"),
                {{QStringLiteral("pull_request"), task.pullRequest},
                 {QStringLiteral("merge_commit"), commit}},
                updates);
            finished = true;
            continue;
        }
        // Open: nothing to say. Closed unmerged or unknown: said once, until the reason changes.
        QString note;
        if (fact->state == QStringLiteral("closed"))
            note = QStringLiteral("%1 was closed without being merged; the leader decides what "
                                  "happens to the task.")
                       .arg(task.pullRequest);
        else if (fact->state.isEmpty())
            note = QStringLiteral("Could not check %1: %2")
                       .arg(task.pullRequest,
                            fact->error.isEmpty() ? QStringLiteral("no answer") : fact->error);
        if (note.isEmpty() || lastPullRequestNote(task) == note)
            continue;
        addHistory(task, QStringLiteral("daemon"), pullRequestKind, note,
                   {{QStringLiteral("pull_request"), task.pullRequest},
                    {QStringLiteral("state"), fact->state}});
        updates.append({.kind = QStringLiteral("pull_request_unchecked"),
                        .text = QStringLiteral("%1: %2").arg(task.id, note),
                        .taskId = task.id,
                        .actionable = fact->state == QStringLiteral("closed")});
    }
    if (updates.isEmpty())
        return;
    if (finished) {
        offerMerges(*team, updates);
        noteGoalReached(*team, updates);
    }
    if (commit(*team, before, QStringLiteral("daemon"), updates))
        qWarning("Unable to record the pull requests of team %lld.",
                 static_cast<long long>(teamId));
}

TeamResult TeamCoordinator::closeTask(const qint64 teamId, const QString& taskId,
                                      const QString& evidence, const QString& note,
                                      const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = TeamError{QStringLiteral("team_not_found"),
                                   QStringLiteral("The team no longer exists.")},
                .teamId = teamId};
    if (actorRole(*team, actor) != role::leader)
        return {.error = TeamError{QStringLiteral("role_forbidden"),
                                   QStringLiteral("Only the team's leader closes a task.")},
                .teamId = teamId};
    const Task* found = std::as_const(*team).task(taskId);
    if (!found)
        return {.error = TeamError{QStringLiteral("task_not_found"),
                                   QStringLiteral("The team has no task %1.").arg(taskId)},
                .teamId = teamId};
    if (found->state == state::done)
        return {.error = TeamError{QStringLiteral("invalid_transition"),
                                   QStringLiteral("%1 is done already.").arg(taskId)},
                .teamId = teamId};
    const QString proof = evidence.trimmed();
    if (!isPullRequestUrl(proof) && !isCommit(proof))
        return {.error = TeamError{QStringLiteral("invalid_payload"),
                                   QStringLiteral("Give a pull request URL or a commit (7 to 40 "
                                                  "hexadecimal characters) as the evidence.")},
                .teamId = teamId};
    if (note.trimmed().isEmpty())
        return {.error = TeamError{QStringLiteral("invalid_payload"),
                                   QStringLiteral("Say in a note why the task is done.")},
                .teamId = teamId};
    const Team before = *team;
    Task* task = team->task(taskId);
    QList<Update> updates;
    const QString by = actorName(*team, actor);
    finishTask(*team, *task, actor,
               QStringLiteral("closed by %1 (%2): %3").arg(by, proof, note.trimmed()),
               QStringLiteral("closed"),
               {{QStringLiteral("by"), by},
                {QStringLiteral("evidence"), proof},
                {QStringLiteral("note"), note.trimmed()}},
               updates);
    offerMerges(*team, updates);
    noteGoalReached(*team, updates);
    if (auto error = commit(*team, before, actor, updates))
        return {.error = error, .teamId = teamId};
    return {.error = std::nullopt, .teamId = teamId};
}

void TeamCoordinator::finishTask(Team& team, Task& task, const QString& actor,
                                 const QString& reason, const QString& kind,
                                 const QJsonObject& data, QList<Update>& updates) {
    // Nothing waits for a task that is done: its merge decisions, its place with the reviewer
    // and a review of it in flight, whose answer reviewFinished then ignores.
    for (Decision& decision : team.decisions)
        if (decision.kind == QStringLiteral("merge") && decision.taskId == task.id &&
            decision.state == QStringLiteral("open")) {
            decision.state = QStringLiteral("withdrawn");
            decision.reason = QStringLiteral("%1 is done.").arg(task.id);
            decision.answeredAt = nowIso();
        }
    team.reviewQueue.removeAll(task.id);
    if (team.reviewing == task.id) {
        team.reviewing.clear();
        team.reviewingMessage.clear();
    }
    task.question.clear();
    applyMove(team, task, state::done, actor, reason, updates);
    addHistory(task, actor, kind, reason, data);
}

void TeamCoordinator::noteGoalReached(const Team& team, QList<Update>& updates) {
    const bool allDone = !team.tasks.isEmpty() &&
                         std::all_of(team.tasks.cbegin(), team.tasks.cend(),
                                     [](const Task& task) { return task.state == state::done; });
    if (allDone)
        updates.append({.kind = QStringLiteral("goal_reached"),
                        .text = QStringLiteral("Every task is done. Report to the user once what "
                                               "%1 contains; merging it onward is the user's.")
                                    .arg(team.integrationBranch),
                        .actionable = true});
}

} // namespace ketplus::daemon::teams
