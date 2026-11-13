# ADR 0123: A team's repository can change, and a task can work in its own

Status: accepted (2026-11-13). Amends ADR 0114 (open point 1, §6 and §10) and ADR 0118.

## Context

ADR 0114 binds a team to the repository it was created in: `team.repository` is written once
by `team.create` and every task's branch and worktree are made beside it, from its
integration branch. Nothing could change it afterwards, from the desktop or from a leader's
tool. Teams that outlived their repository were stuck: a team bound to an archived checkout
kept creating task worktrees from it, and its developers worked around it with worktrees of
their own in the repositories the work was really in. Teams whose goal spans several
repositories had the same problem for single tasks.

## Decision

1. **The team's repository is a setting.** `team.update` takes `repository` (the full path
   of a main checkout) with an optional `integration_branch` and `reason`; the desktop Team
   page offers it as **Repository…** in the team's menu, and the leader has the tool
   `team_repository_set {repository, integration_branch?, reason}`, where the reason is
   required, as for a lane change. A paired phone cannot change it (its `team.update`
   whitelist is unchanged).
2. **Validation.** The daemon refuses with `invalid_repository` and a sentence a person can
   act on unless the path is absolute, is a folder, is a Git repository, is that
   repository's main checkout (not a subfolder or a linked worktree), and, when the team has
   an integration branch, has a remote named `origin` with that branch as a remote-tracking
   ref (`refs/remotes/origin/<branch>`). Nothing is fetched: a new branch is fetched first.
   The integration branch can change with the repository but not be removed, as before.
   The same repository and branch again is no change and records nothing.
3. **Started tasks keep where they began.** A task stores `repository` and
   `integration_branch` of its own when they are not the team's. When the team moves, every
   task that already has a branch (done ones too, whose worktrees are still cleaned up)
   keeps the old repository and integration branch, with a `repository` entry in its
   history. Tasks not started yet and new tasks use the new repository. Nothing on disk is
   moved, deleted or reset; branches and worktrees keep their names. A worktree git made
   while the team moved is kept with the repository it was made in.
4. **A task may name its own repository.** `team_task_create` takes optional `repository`
   and `integration_branch` (the team's branch by default), checked as in point 2. Naming
   the team's own repository and branch stores nothing. The task dialog shows it.
5. **Git about a task reads the task's scope.** `teams::taskScope(team, task)` is the team
   with the task's repository and integration branch when it has them. The worktree and
   branch, the hand to review, the review's diff, lanes, the merge check and the fetch of
   origin's integration branch (once per repository and branch), the pull request comment,
   the merge texts and the cleanup of a done task's worktree (its main-checkout guard and
   the folder its conversations move back to) all use it. The pull request probe passes
   full URLs, which do not depend on the folder it runs in.
6. **Recorded and visible.** The change is a team update of kind `repository` naming both
   repositories and branches, the reason and the tasks that kept theirs (`data: {from, to,
   from_branch, to_branch, reason, kept}`), shown in the board's Updates. The journal's
   `teams.repository` column follows the document, so the team is listed under its new
   repository.

7. **A member's conversation follows its task.** Assigning a task already points the owner
   at the task's worktree. A message that starts a developer's turn on its task in progress,
   such as a review's findings or an answered question, now points it there as well, so
   Source Control, Files and the worktree picker show the work in progress even when the
   conversation was left on another worktree meanwhile. When a task becomes done, an owner
   whose conversation stands in its worktree is moved out (the relocation of ADR 0114 §7):
   to its task in progress, else another open task, else the team's repository, once its
   turn allows.
8. **Work in a second repository.** Work that belongs in another repository is a task of its
   own there (`team_task_create` with `repository`), so the daemon makes and tracks its
   worktree and the member is pointed at it. A developer that still had to change another
   worktree hands the task to review with that `pull_request` and `head_commit` (ADR 0118)
   and names the worktree in the note; the developer instructions say so.

Members are not moved when the team moves: a conversation that joins later must work in the
team's current repository, as ADR 0114 requires, and a member's next task points it at that
task's worktree.

## Consequences

- A team stuck on an archived repository is moved by its leader or the user without losing
  the work in flight; the tasks already started finish where they are.
- Teams that span repositories keep one team and name the exception per task.
- Every place that runs git for a task must read `taskScope`; reading `team.repository`
  directly is a bug for a task with its own.
- Validation runs a few short local git commands on the runtime thread, like
  `conversationRepository` does when a member joins.

## Alternatives considered

- **Moving open tasks with the team.** Rejected: their worktrees and branches live in the old
  repository; moving them means recreating branches or force-resetting, which loses work and
  breaks open pull requests.
- **Checking origin over the network (`git ls-remote`).** Rejected for the setting: it blocks
  on the network and on credentials; the remote-tracking ref is what worktrees are made from
  anyway.
- **One team per repository only.** Rejected: real goals span an app, its kit and its web
  site, and a second team per repository splits one plan.
