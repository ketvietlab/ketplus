// T-38: which commit is a task's work. A task gets a head only from a commit made on its own
// branch after its base; a worktree checked out at its base, at the integration branch or at
// another task's commit gives it none. Work done in another worktree or repository is named
// by the hand to review: the pull request and, if known, its head commit (ADR 0114 §2).
#include "teams/TeamCoordinator.h"

#include <QRegularExpression>

namespace ketplus::daemon::teams {
namespace {

bool isCommitId(const QString& text) {
    static const QRegularExpression commit(QStringLiteral("^[0-9a-fA-F]{7,40}$"));
    return commit.match(text).hasMatch();
}

// Another task of the team was handed to review at this commit, or its branch is there.
bool anotherTasksCommit(const Team& team, const Task& task, const QString& commit) {
    for (const Task& other : team.tasks)
        if (other.id != task.id && !other.headCommit.isEmpty() && other.headCommit == commit)
            return true;
    return false;
}

} // namespace

bool TeamCoordinator::branchHasOwnCommits(const Task& task, const QString& branchHead) {
    return !branchHead.isEmpty() && !task.baseCommit.isEmpty() && branchHead != task.baseCommit;
}

bool TeamCoordinator::reviewedOwnCommit(const Task& task) {
    return branchHasOwnCommits(task, task.headCommit);
}

std::optional<TeamError> TeamCoordinator::attributeHead(const Team& team, Task& task,
                                                        const QString& readHead,
                                                        const QString& reportedHead) {
    const QString reported = reportedHead.trimmed();
    if (!reported.isEmpty() && !isCommitId(reported))
        return TeamError{
            QStringLiteral("invalid_payload"),
            QStringLiteral("head_commit is a commit: 7 to 40 hexadecimal characters.")};
    QString head = readHead;
    if (head == task.baseCommit || anotherTasksCommit(team, task, head))
        head.clear();
    if (!head.isEmpty()) {
        // The branch's own commit is what is reviewed, whatever the hand to review names.
        task.headCommit = head;
        return std::nullopt;
    }
    if (!recordsPullRequest(task))
        return TeamError{
            QStringLiteral("no_commits"),
            QStringLiteral("%1 has no commit of its own on %2 after its base %3. Commit your "
                           "work on that branch, or, for work done in another worktree or "
                           "repository, hand it to review with its pull_request and head_commit.")
                .arg(task.id, task.branch.isEmpty() ? QStringLiteral("its branch") : task.branch,
                     task.baseCommit.left(12))};
    // Work outside the branch: the board reviews the pull request at the commit it names, and
    // only the pull request makes the task done (ADR 0118).
    task.headCommit = reported;
    addHistory(
        task, QStringLiteral("daemon"), QStringLiteral("handoff"),
        reported.isEmpty()
            ? say(team.language,
                  QT_TRANSLATE_NOOP("Team", "Handed to review with work outside its branch: %1."))
                  .arg(task.pullRequest)
            : say(team.language,
                  QT_TRANSLATE_NOOP("Team",
                                    "Handed to review with work outside its branch: %1 at %2."))
                  .arg(task.pullRequest, reported.left(12)),
        {{QStringLiteral("pull_request"), task.pullRequest},
         {QStringLiteral("head_commit"), reported}});
    return std::nullopt;
}

} // namespace ketplus::daemon::teams
