# ADR 0118: A team task is done when its pull request is merged

Status: accepted (2026-11-12). Amends ADR 0114 (open point 3 and §6).

## Context

ADR 0114 decides that a task is done only when local git says the team repository's
integration branch contains the task's branch, and it rejects asking the hosting service.
Teams that merge through pull requests showed three faults:

- A merge through a pull request is invisible while the local integration branch lags the
  remote, so a whole team finished with no task done.
- When the work lives in another repository than the one the team is registered on, the team
  branch has no commit of its own: its head is its base, which every integration branch
  contains, so the task was done the moment it was ready while its pull request was open.
- A dependent task is refused with `dependencies_open` until its dependency is done, so with
  the first fault nothing that depends on merged work could start.

The leader had no way to close a task either.

## Decision

1. **A recorded pull request decides.** A task that records a pull request URL becomes done
   when that pull request is merged on the remote, from every state that is not done (To do,
   In progress, In review, Ready, Blocked), whatever repository it is in. A squash or rebase
   merge counts like a merge commit. The `state` history entry and a `merged` entry name the
   pull request and the merge commit (`data: {pull_request, merge_commit}`). The task's open
   merge decisions are withdrawn, it leaves the review queue, and a review of it in flight is
   forgotten, so the reviewer's late answer changes nothing.
2. **The probe.** The daemon asks with the GitHub command line tool, `gh pr view <url> --json
   state,mergeCommit`, the tool the app's agents already sign in with. It runs on a worker of
   its own, apart from git's, so a slow network holds no merge check, review diff or
   worktree. Each request has 15 s and a batch 45 s; the first request that does not answer
   ends the batch, and the pull requests not asked wait for the next check (they are not
   errors). A daemon that stops ends the batch at once. Prompts are disabled, it runs in the
   team repository's directory and never touches a checkout. It sits behind `TeamHostPort::askPullRequests`, which the tests
   fake. **Without it** (no `gh`, signed out, offline, an unknown URL) every pull request is
   unknown: no task moves, the task's history and Updates say why once, and say it again only
   when the reason changes. The leader then closes the task by hand (point 5). A closed,
   unmerged pull request is said once as well, actionable for the leader; the task stays.
3. **Bounds.** A check asks at most 20 distinct pull requests, one request each however many
   tasks share it, and the next page the next time. A team is asked at most once a minute.
   Checks run when the team is checked for merges (a member's turn ends, a client reads the
   team), every 2 minutes on a timer, and once after the daemon loads, so work merged while
   the board lagged or the daemon was stopped is reconciled.
4. **Git's rule is narrowed.** The ancestor check of ADR 0114 still makes a Ready task done,
   now only for a task that records no pull request, and only when the task's branch has
   commits of its own (its head is not its base). A task with a pull request is the pull
   request's alone: its branch can be contained without its work being merged, after it took
   in the integration branch or when the work is in another repository. Before
   asking, the daemon fetches the integration branch from `origin` into
   `refs/remotes/origin/<integration>` (at most once a minute per repository, no tags, no
   `FETCH_HEAD`, prompts off and ssh in batch mode unless `GIT_SSH_COMMAND` is set), and
   accepts the local or the remote-tracking branch. The fetch
   changes no local branch, no checkout and no file.
5. **The leader closes a task by hand.** `team_task_close` takes the task, the evidence (a
   pull request URL or a commit of 7 to 40 hexadecimal characters) and a note. Every other
   role is refused with `role_forbidden`. A `closed` history entry records who closed it,
   the evidence and the note.
6. **Dependencies.** A task done by any of these ways satisfies its dependents at once; the
   leader does nothing more.
7. **Contract.** The new entries are ordinary history entries `{at, actor, kind, text}` of
   ADR 0116, with new `kind` values `merged`, `closed` and `pull_request`, and an Updates kind
   `pull_request_unchecked`. No field is added; clients that show history as text show them.

## Consequences

- Teams that merge through pull requests reach done, and their dependents start, with no
  action by the leader. A task with an open pull request and an untouched team branch stays
  where it is.
- The daemon now calls a hosting service when a task records a pull request URL. It uses the
  user's own `gh` sign-in; it stores no credential.
- A daemon without `gh` behaves as before for local merges, says once why it cannot check a
  pull request, and relies on the leader's tool.

## Alternatives considered

- *Git against the remote only*: fetch the pull request's base and test ancestry. It sees
  neither a squash merge nor a pull request in another repository than the team's.
- *GitHub's REST API from the daemon*: needs its own token storage; `gh` already has one.
- *Leave done to the leader*: every team would need a manual close per task, and a lagging
  board would still block dependents.
