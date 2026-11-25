// ADR 0118: a task is done when its pull request is merged on the remote, or when the leader
// closes it by hand. Git's own rule (TeamCoordinatorMerges.cpp) stays for tasks merged locally.
#include "teams/TeamCoordinator.h"

#include <QDateTime>
#include <QDir>
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

// The state the board last saw the task's pull request in, from its history entries.
QString lastPullRequestState(const Task& task) {
    for (auto it = task.history.crbegin(); it != task.history.crend(); ++it)
        if (it->kind == pullRequestKind &&
            !it->data.value(QStringLiteral("state")).toString().isEmpty())
            return it->data.value(QStringLiteral("state")).toString();
    return {};
}

} // namespace

bool TeamCoordinator::recordsPullRequest(const Task& task) {
    return isPullRequestUrl(task.pullRequest);
}

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
    // T-3: a task with work and a branch but no pull request may have one a workflow opened.
    QList<Task> unlinked;
    for (const Task& task : team->tasks)
        if (task.state != state::done && task.state != state::todo &&
            task.pullRequest.trimmed().isEmpty() && !task.branch.trimmed().isEmpty())
            unlinked.append(task);
    if (urls.isEmpty() && unlinked.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto last = pullRequestCheckedAt_.constFind(teamId);
    if (last != pullRequestCheckedAt_.cend() && now - *last < pullRequestGap_)
        return;
    pullRequestCheckedAt_.insert(teamId, now);
    if (!unlinked.isEmpty() && !pullRequestLinks_.contains(teamId)) {
        pullRequestLinks_.insert(teamId);
        host_->askPullRequestLinks(
            *team, unlinked,
            [self = QPointer<TeamCoordinator>(this), teamId](const QHash<QString, QString>& links) {
                if (!self)
                    return;
                self->pullRequestLinks_.remove(teamId);
                self->applyPullRequestLinks(teamId, links);
            });
    }
    if (urls.isEmpty())
        return;
    // Bounded: one request per pull request, at most a page of them per check. The page starts
    // where the last check stopped asking, in sorted order and wrapping around, so the board is
    // gone through in turn and a pull request that never answers (it ends its batch) is asked
    // again only after every other one was: each check asks at least one, so every pull
    // request is asked within as many checks as there are pull requests.
    std::sort(urls.begin(), urls.end());
    const QString cursor = pullRequestCursor_.value(teamId);
    const int start =
        static_cast<int>(std::lower_bound(urls.cbegin(), urls.cend(), cursor) - urls.cbegin()) %
        static_cast<int>(urls.size());
    QStringList page;
    for (int i = 0; i < std::min<int>(maxPullRequestsPerCheck, urls.size()); ++i)
        page.append(urls.at((start + i) % urls.size()));
    // The one after the page, for a batch that asks every one of it.
    const QString afterPage = urls.at((start + page.size()) % urls.size());
    pullRequestChecks_.insert(teamId);
    host_->askPullRequests(*team, page,
                           [self = QPointer<TeamCoordinator>(this), teamId, page,
                            afterPage](const QHash<QString, PullRequestFact>& facts) {
                               if (!self)
                                   return;
                               self->pullRequestChecks_.remove(teamId);
                               // A batch that stopped early leaves the rest out: the next check
                               // starts at the first one it did not ask.
                               QString next = afterPage;
                               for (const QString& url : page)
                                   if (!facts.contains(url)) {
                                       next = url;
                                       break;
                                   }
                               self->pullRequestCursor_.insert(teamId, next);
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
        // T-3: a pull request that is open again after it was closed wakes the leader.
        const QString seen = lastPullRequestState(task);
        if (fact->state == QStringLiteral("open") && seen == QStringLiteral("closed")) {
            const QString text = QStringLiteral("%1 is open again.").arg(task.pullRequest);
            addHistory(task, QStringLiteral("daemon"), pullRequestKind, text,
                       {{QStringLiteral("pull_request"), task.pullRequest},
                        {QStringLiteral("state"), QStringLiteral("open")}});
            updates.append({.kind = QStringLiteral("pull_request_changed"),
                            .text = QStringLiteral("%1: %2").arg(task.id, text),
                            .taskId = task.id,
                            .actionable = true});
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

void TeamCoordinator::applyPullRequestLinks(const qint64 teamId,
                                            const QHash<QString, QString>& links) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return;
    const Team before = *team;
    QList<Update> updates;
    for (Task& task : team->tasks) {
        const auto link = links.constFind(task.id);
        // Only a task still without one: the person's own record is never replaced.
        if (link == links.cend() || task.state == state::done ||
            !task.pullRequest.trimmed().isEmpty() || !isPullRequestUrl(*link))
            continue;
        task.pullRequest = link->trimmed();
        const QString text = QStringLiteral("%1 was opened from branch %2 and is linked.")
                                 .arg(task.pullRequest, task.branch);
        addHistory(task, QStringLiteral("daemon"), pullRequestKind, text,
                   {{QStringLiteral("pull_request"), task.pullRequest},
                    {QStringLiteral("state"), QStringLiteral("open")}});
        updates.append({.kind = QStringLiteral("pull_request_linked"),
                        .text = QStringLiteral("%1: %2").arg(task.id, text),
                        .taskId = task.id,
                        .actionable = true});
    }
    if (updates.isEmpty())
        return;
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
            decision.reason =
                say(team.language, QT_TRANSLATE_NOOP("Team", "%1 is done.")).arg(task.id);
            decision.answeredAt = nowIso();
        }
    dropReview(team, task.id);
    task.question.clear();
    const qsizetype stateUpdate = updates.size();
    applyMove(team, task, state::done, actor, reason, updates);
    updates[stateUpdate].data.insert(QStringLiteral("completion"), kind);
    if (kind == QStringLiteral("merged")) {
        // The state change is routine. The external observation is what wakes the leader.
        updates[stateUpdate].actionable = false;
        updates.append({.kind = QStringLiteral("merge_observed"),
                        .text = QStringLiteral("%1: %2").arg(task.id, reason),
                        .taskId = task.id,
                        .actionable = true,
                        .actor = actor});
    }
    addHistory(task, actor, kind, reason, data);
    // T-90: the owner does not stay in the worktree of work that is in. It moves to its task
    // in progress, another open task, or the team's repository (relocationHome), once the
    // change is stored and its turn allows.
    if (const auto conversation = task.owner.isEmpty() || task.worktree.isEmpty()
                                      ? std::nullopt
                                      : host_->teamConversation(task.owner);
        conversation && team.member(task.owner) &&
        QDir::cleanPath(conversation->worktree) == QDir::cleanPath(task.worktree)) {
        const Relocation move{.session = task.owner, .from = task.worktree};
        if (!team.relocations.contains(move))
            team.relocations.append(move);
        QTimer::singleShot(0, this, [this, teamId = team.id, session = task.owner] {
            settleRelocations(teamId, session, {});
        });
    }
}

void TeamCoordinator::noteGoalReached(const Team& team, QList<Update>& updates) {
    const bool allDone = !team.tasks.isEmpty() &&
                         std::all_of(team.tasks.cbegin(), team.tasks.cend(),
                                     [](const Task& task) { return task.state == state::done; });
    if (allDone)
        updates.append({.kind = QStringLiteral("goal_reached"),
                        .text = say(team.language,
                                    QT_TRANSLATE_NOOP(
                                        "Team", "Every task is done. Report to the user once what "
                                                "%1 contains; merging it onward is the user's."))
                                    .arg(team.integrationBranch),
                        .actionable = true,
                        .actor = QStringLiteral("daemon")});
}

} // namespace ketplus::daemon::teams
