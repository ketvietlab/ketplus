#pragma once

// A fake runtime for the team coordinator: conversations, worktrees and the messages a team
// sends, all in memory (ADR 0114).
#include "teams/TeamCoordinator.h"

#include <QDir>
#include <QHash>
#include <QSet>

#include <functional>
#include <utility>

namespace team_test {

using namespace ketplus::daemon::teams;

class FakeHost final : public TeamHostPort {
  public:
    std::optional<TeamConversation> teamConversation(const QString& sessionId) const override {
        const auto it = conversations.constFind(sessionId);
        return it == conversations.cend() ? std::nullopt : std::optional(it.value());
    }
    std::optional<TeamConversation> conversationByNumber(const QString& number) const override {
        for (const TeamConversation& conversation : conversations)
            if (conversation.number == number)
                return conversation;
        return std::nullopt;
    }
    QString conversationRepository(const QString& sessionId) const override {
        return conversations.contains(sessionId) ? repository : QString{};
    }
    std::optional<TaskWorktree> createTaskWorktree(const Team& team, const Task& task,
                                                   QString* error) override {
        ++made;
        if (!worktreeError.isEmpty()) {
            *error = worktreeError;
            return std::nullopt;
        }
        // T-90: made in the repository the task works in, as team_git::createWorktree does.
        const Team scope = taskScope(team, task);
        madeIn.append(scope.repository);
        return TaskWorktree{.branch = QStringLiteral("team/%1/%2").arg(team.id).arg(task.id),
                            .worktree = worktreeRoot + task.id,
                            .baseCommit = QStringLiteral("base"),
                            .repository = scope.repository,
                            .integrationBranch = scope.integrationBranch};
    }
    RepositoryCheck checkRepository(const QString& path,
                                    const QString& integrationBranch) const override {
        repositoryChecks.append({path, integrationBranch});
        if (const QString error = repositoryErrors.value(path); !error.isEmpty())
            return {.repository = {}, .error = error};
        return {.repository = QDir::cleanPath(path), .error = {}};
    }
    // T-93: the branches git already has, by repository and name.
    std::optional<bool> branchTaken(const QString& repository,
                                    const QString& branch) const override {
        if (unlistedRepositories.contains(repository))
            return std::nullopt;
        return takenBranches.contains(repository + QLatin1Char('|') + branch);
    }
    QString pointConversationAt(const QString& sessionId, const QString& worktree) override {
        if (const QString error = pointErrors.value(sessionId); !error.isEmpty())
            return error;
        pointed.insert(sessionId, worktree);
        if (conversations.contains(sessionId))
            conversations[sessionId].worktree = worktree;
        return {};
    }
    std::optional<bool> hasUncommittedWork(const QString& worktree) const override {
        ++statusReads;
        return dirty.contains(worktree);
    }
    QString taskCommit(const Team&, const Task& task) const override {
        return heads.value(task.branch, task.branch + QStringLiteral("@head"));
    }
    std::optional<QString> ownHead(const Team& team, const Task& task) const override {
        if (unreadableHeads.contains(task.branch))
            return std::nullopt;
        return TeamHostPort::ownHead(team, task);
    }
    std::optional<TaskDiff> taskDiff(const Team& team, const Task& task) const override {
        (void)team;
        if (unreadable.contains(task.id))
            return std::nullopt;
        return TaskDiff{.diff = QStringLiteral("+++ b/web/%1.txt\n+change of %1").arg(task.id),
                        .files = {QStringLiteral("web/%1.txt").arg(task.id)}};
    }
    std::optional<QStringList> changedPaths(const Team& team, const Task& task) const override {
        (void)team;
        const auto it = changed.constFind(task.id);
        return it == changed.cend() ? std::nullopt : std::optional(it.value());
    }
    std::optional<bool> isMerged(const Team& team, const Task& task) const override {
        (void)team;
        return merged.contains(task.id);
    }
    QString postTeamMessage(const TeamMessage& message) override {
        messages.append(message);
        return {};
    }
    void noteInConversation(const QString& sessionId, const QJsonObject& note) override {
        notes.append({sessionId, note});
    }
    void releaseTeamHeldMessages(const QStringList& sessionIds) override {
        released.append(sessionIds);
    }
    // With `holdAnswers`, git is read when it is asked and the answer waits for answer(), as
    // the session runtime answers a check after the caller's turn.
    void askTasks(const Team& team, const QList<Task>& tasks, const TaskQuestion question,
                  TaskFactsDone done) override {
        ++asked;
        if (!holdAnswers) {
            TeamHostPort::askTasks(team, tasks, question, std::move(done));
            return;
        }
        TeamHostPort::askTasks(team, tasks, question,
                               [this, done](const QHash<QString, TaskFacts>& facts) {
                                   held.append([done, facts] { done(facts); });
                               });
    }
    void askDiff(const Team& team, const Task& task, TaskDiffDone done) override {
        ++asked;
        if (!holdAnswers) {
            done(taskDiff(team, task));
            return;
        }
        held.append([done, diff = taskDiff(team, task)] { done(diff); });
    }
    // A prepared command's git: done when it is asked, as git works while the caller waits.
    void askWorktree(const Team& team, const Task& task, WorktreeDone done) override {
        ++asked;
        TeamHostPort::askWorktree(team, task, [this, done](const WorktreeAnswer& answer) {
            if (holdAnswers)
                held.append([done, answer] { done(answer); });
            else
                done(answer);
        });
    }
    void askHandoff(const Team& team, const Task& task, HandoffDone done) override {
        ++asked;
        TeamHostPort::askHandoff(team, task, [this, done](const HandoffFacts& facts) {
            if (holdAnswers)
                held.append([done, facts] { done(facts); });
            else
                done(facts);
        });
    }
    // ADR 0118: the remote's pull requests. One not in `pullRequests` is open; held like git
    // with `holdAnswers`. Counted apart from git, so a test of git's checks is unchanged.
    void askPullRequests(const Team& team, const QStringList& urls,
                         PullRequestsDone done) override {
        (void)team;
        pullRequestAsks.append(urls);
        QHash<QString, PullRequestFact> facts;
        for (const QString& url : urls) {
            // As the probe does: one that does not answer is an error and ends the batch.
            if (hangingPullRequests.contains(url)) {
                facts.insert(url, {.state = {}, .mergeCommit = {}, .error = "timed out"});
                break;
            }
            facts.insert(
                url, pullRequests.value(
                         url, {.state = QStringLiteral("open"), .mergeCommit = {}, .error = {}}));
        }
        if (holdAnswers)
            held.append([done, facts] { done(facts); });
        else
            done(facts);
    }
    // T-40: a review on the pull request posts its findings there; the comment gets a link
    // unless `commentError` is set. Held like git with `holdAnswers`.
    void askPostReviewComment(const Team& team, const Task& task, const QString& body,
                              ReviewCommentDone done) override {
        (void)team;
        comments.append({task.pullRequest, body});
        const ReviewComment answer = commentError.isEmpty()
                                         ? ReviewComment{.url = QStringLiteral("%1#issuecomment-%2")
                                                                    .arg(task.pullRequest)
                                                                    .arg(comments.size()),
                                                         .error = {}}
                                         : ReviewComment{.url = {}, .error = commentError};
        if (holdAnswers)
            held.append([done, answer] { done(answer); });
        else
            done(answer);
    }
    // ADR 0114 §10: the disk under the team and a done task's worktree removal. The removal
    // deletes the folder unless `removalError` says why it failed.
    void askStorage(const QString& volumePath, const QStringList& worktrees,
                    StorageDone done) override {
        storageAsks.append({volumePath, worktrees});
        done(storageFacts);
    }
    void askRemoveWorktree(const QString& repository, const QString& worktree,
                           RemovalDone done) override {
        removals.append({repository, worktree});
        const auto finish = [done, worktree, error = removalError] {
            if (error.isEmpty())
                QDir(worktree).removeRecursively();
            done(error);
        };
        if (holdAnswers)
            held.append(finish);
        else
            finish();
    }
    QString conversationRunningIn(const QString& path) const override {
        return runningIn.value(path);
    }
    QList<FolderConversation> conversationsIn(const QString& path) const override {
        QList<FolderConversation> found;
        for (const QString& id : inFolder.value(path))
            found.append({.sessionId = id, .title = conversations.value(id).title});
        return found;
    }
    void answer() {
        const QList<std::function<void()>> waiting = std::exchange(held, {});
        for (const auto& one : waiting)
            one();
    }

    void add(const QString& id, const QString& provider = QStringLiteral("codex")) {
        conversations.insert(id, {.sessionId = id,
                                  .generation = 1,
                                  .number = id,
                                  .title = id,
                                  .provider = provider,
                                  .idle = true});
    }
    void setIdle(const QString& id, const bool idle) { conversations[id].idle = idle; }
    [[nodiscard]] QList<TeamMessage> of(const QString& kind) const {
        QList<TeamMessage> result;
        for (const TeamMessage& message : messages)
            if (message.kind == kind)
                result.append(message);
        return result;
    }

    QHash<QString, TeamConversation> conversations;
    QHash<QString, QString> pointed;
    QHash<QString, QString> heads;
    // T-38: branches git cannot read.
    QSet<QString> unreadableHeads;
    QHash<QString, PullRequestFact> pullRequests;
    QList<QStringList> pullRequestAsks;
    QSet<QString> dirty;
    QSet<QString> unreadable;
    QSet<QString> hangingPullRequests;
    // T-40: the comments posted, as {pull request, body}.
    QList<std::pair<QString, QString>> comments;
    QString commentError;
    QHash<QString, QStringList> changed;
    QSet<QString> merged;
    bool holdAnswers = false;
    int asked = 0;
    // Worktrees made and worktrees read, however they were asked for.
    int made = 0;
    mutable int statusReads = 0;
    QString worktreeError;
    // Where the worktrees it makes are; a test that needs them on disk points it at a folder.
    QString worktreeRoot = QStringLiteral("/wt/");
    // The repository its conversations are in.
    QString repository = QStringLiteral("/repo");
    // T-90: the repository each worktree was made in, the repositories checked, and the
    // ones refused with the reason.
    QStringList madeIn;
    mutable QList<std::pair<QString, QString>> repositoryChecks;
    QHash<QString, QString> repositoryErrors;
    // T-93: "<repository>|<branch>" of branches git has.
    QSet<QString> takenBranches;
    // T-93 review L1: repositories whose branches were not listed yet.
    QSet<QString> unlistedRepositories;
    StorageFacts storageFacts;
    QList<std::pair<QString, QStringList>> storageAsks;
    QList<std::pair<QString, QString>> removals;
    QString removalError;
    QHash<QString, QString> runningIn;
    QHash<QString, QStringList> inFolder;
    // A conversation that refuses to be pointed elsewhere, with the reason.
    QHash<QString, QString> pointErrors;
    QList<std::function<void()>> held;
    QList<TeamMessage> messages;
    QList<std::pair<QString, QJsonObject>> notes;
    QStringList released;
};

// The first reviewer's share of the reviews (T-23), empty when the team has no reviewer.
inline ReviewSlot firstReviews(const Team& team) {
    return team.reviews.isEmpty() ? ReviewSlot{} : team.reviews.first();
}

// The reviewer `session` answers the request its review in flight is bound to.
inline void finishReview(TeamCoordinator& teams, const QString& session, const QString& text,
                         const bool noFindings, const QString& failure = {}) {
    const Team* team = teams.teamOf(session);
    const ReviewSlot* slot = team ? team->reviewSlot(session) : nullptr;
    teams.reviewFinished(session, slot ? slot->message : QString{}, text, noFindings, failure);
}

inline Team standardTeam() {
    Team team;
    team.repository = QStringLiteral("/repo");
    team.name = QStringLiteral("Checkout");
    team.integrationBranch = QStringLiteral("integration");
    // The first plan is approved unless a test is about it (ADR 0114, open point 5).
    team.planApproved = true;
    team.members = {{.sessionId = "lead", .role = role::leader, .name = "Lead"},
                    {.sessionId = "dev1",
                     .role = role::developer,
                     .name = "Dev 1",
                     .lane = "Web",
                     .paths = {"web"}},
                    {.sessionId = "dev2",
                     .role = role::developer,
                     .name = "Dev 2",
                     .lane = "Api",
                     .paths = {"api"}},
                    {.sessionId = "rev", .role = role::reviewer, .name = "Reviewer"}};
    return team;
}

} // namespace team_test
