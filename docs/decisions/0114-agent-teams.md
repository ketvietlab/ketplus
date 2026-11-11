# ADR 0114: Agent teams — a leader, developers and a reviewer on one goal

Status: accepted (Jay, 2026-11-10 for the rules marked *decided*; the open points
below were fixed in this ADR with the recommendation of epic #450). Implements
ketvietlab/ketplus#450; this record is #454.

See also: ADR 0027 (agent messaging), ADR 0067 (Reviewer agent), ADR 0030
(integration branch), ADR 0107 (conversation tags), ADR 0111 (worktrees belong to
conversations). Design reference: the `teams` screen of `mockups/qt` (#451).

## Context

Conversations already coordinate through agent messaging (ADR 0027). Messaging gives
them a way to talk and nothing else: no task has an owner, nobody has a role, there is
no shared view of the work, and the user is asked the same question in several
transcripts. A **team** adds those four things — roles, tasks, one place for decisions
and a digest instead of status messages — and leaves messaging as it is.

## Decision

### 1. The model (decided)

- A **team** belongs to one repository and has a name, a goal, an integration branch,
  a merge setting (`mergePolicy`), `leaderWritesRequirements`, a `paused` switch,
  shared paths, members, tasks, decisions and updates. The daemon stores it.
- Exactly **one leader**, **one to five developers** and exactly **one reviewer**.
  A conversation is on at most one team. Members belong to the team's repository.
  *Rejected*: any number of reviewers or leaders — two leaders split the plan, and a
  second reviewer only hides a queue that a second team removes.
- The **leader** is the only member that reports to the user. It breaks the goal into
  tasks, writes their acceptance criteria (unless `leaderWritesRequirements` is off),
  assigns them, changes lanes and orders the merges. It neither writes code nor
  reviews.
- A **developer** owns a lane: a name and path prefixes.
- The **reviewer** reviews each task against its criteria. It owns no task, so it never
  reviews its own work, and it never merges.
- **No new authority**: nothing a member sends approves a permission request, answers a
  decision or merges a protected branch. ADR 0027 is unchanged; members may address
  `leader` and `reviewer` by role as well as by number.

### 2. The reviewer reuses ADR 0067 and reviews per task (decided)

One engine, a second trigger. A task entering `in_review` sends the team's reviewer a
`review` message with the task's criteria and the diff of the task's branch against the
integration branch it was cut from. The verdict line of ADR 0067 decides: `no findings`
makes the task `ready`; `findings`, or a reply without the line, returns it to its owner
with round + 1. Past `max_rounds` (default 2, 1 to 3) the task is `blocked` and the
leader's digest says why. A busy reviewer **queues** requests first in, first out,
instead of skipping them. The reviewer is therefore a Claude conversation, the only
provider that enforces the read-only guard. A member's own Reviewer setting stays
stored but does not run while it is on a team, and `session.reviewer.set` is refused
with `team_member`. *Rejected*: a second review engine — it would drift from 0067.

### 3. Decisions live on the team page (decided)

A question that changes the product is one **decision** on the team: question, context,
two to four choices, the leader's recommendation, the task it blocks. Only the leader
raises one; the daemon raises its own (the first plan, a used-up budget, a merge that
waits for the user, an empty role). The answer is the user's, sent by a client, and is
delivered once to the leader and to the owner of the blocked task as a `decision`
message. The team page shows it first; the team's folder shows the count; the leader's
row shows the "Waiting for you" status of #125. *Rejected*: an app-wide inbox — a
second place to look for the same question.

### 4. Lane changes by the leader are logged (decided)

The leader changes a lane itself with `team_lane_set` (the user can from the team page).
The change goes to Updates with the old and new prefixes and the reason, and to the
history of every open task of the lanes it touches. It starts no turn. The leader raises
a decision only when a change alters what the product does. A prefix belongs to one
lane (`lane_overlap`). After a developer's turn the daemon compares the files changed on
the task's branch with the owner's prefixes and the shared paths, records the outside
paths on the task (`outsideLane`, actionable in the digest) and clears the flag when they
are reverted or covered. *Rejected*: asking the user for every lane change — it is
about who edits which file, not what the product does.

### 5. Who merges a reviewed task is a team setting (decided)

`mergePolicy` is `leader` (default) or `user`. With `leader`, the leader merges a
`ready` task whose dependencies are `done`. With `user`, a `ready` task raises a
decision "Merge T-n?"; choosing "Let the leader merge this one" allows exactly that
merge. Changing the setting takes effect for the next task that becomes `ready` and is
logged. Merging the integration branch onward always stays with the user.

### 6. Tasks

- Id `T-n` within the team, never reused. Title, description, lane, dependencies,
  ordered acceptance criteria with a met mark, at most one owner (a developer), branch,
  worktree, pull request, review round, `outsideLane`.
- States: `todo`, `in_progress`, `in_review`, `ready`, `blocked`, `done`.

| Move | Who |
| --- | --- |
| → `todo` (create, unassign) | leader, user |
| `todo`/`blocked` → `in_progress` | owner (dependencies `done`) |
| `in_progress` → `in_review` | owner, work committed |
| `in_progress`/`todo` → `blocked` | owner, daemon |
| `in_review` → `ready` / `in_progress` / `blocked` | daemon (review) |
| `ready` → `done` | daemon (ancestor check) |

- A developer has at most one task `in_progress`. Only the leader (or the user) edits
  the text of a criterion; the owner sets the mark. The history is append-only.
- A handover changes the owner and needs a reason; the worktree, branch and history
  move with the task. There is never a second owner, also when a handover fails.

### 7. What wakes whom

| Event | Starts a turn of |
| --- | --- |
| assignment or handover | the new owner (`assignment` message) |
| task enters `in_review` | the reviewer (`review`) |
| review with findings | the owner (`review` reply) |
| decision answered | the leader and the blocked owner (`decision`) |
| actionable updates | the leader, once, as a `digest` |
| anything else | nobody |

Actionable updates: a task became `ready`, `blocked` or `done`, a review hit its round
limit, a path was flagged outside a lane, a developer has no task left, a developer's
question. When the leader's turn ends with actionable updates waiting, or after a quiet
period of **20 s** while it is idle, the daemon sends one digest of every update since
the last one, grouped by task. Other updates ride along in the next digest.

## Open points, fixed here

1. **A developer's working directory.** *Chosen*: a task owns its worktree. When a task
   is first assigned the daemon creates branch `team/<team-id>/t-<n>` from the head
   of the integration branch, in a worktree beside the repository as ADR 0111 places
   them (`<repository>-team-<team-id>-t-<n>`), and
   re-points the owner's conversation at it with the worktree switch of ADR 0111 before
   the assignment turn starts. A handover re-points the new owner. *Rejected*: one
   worktree per developer switching branches — a handover would have to move
   uncommitted work between worktrees, and a developer could carry it into the wrong
   task.
2. **The automatic-turn budget.** *Chosen*: a team may start **40 automatic turns** of
   its members since the user last acted on it (created, resumed or answered a
   decision). When it runs out, the team pauses itself and raises one decision:
   "Continue (40 more turns)" or "Stop". The loop guard of ADR 0027 (hop 6, 30 turns a
   chain, 20 an hour per conversation) applies to every automatic turn on top. Team
   messages start their own chain, so an `agents_send` loop inside a team still stops at
   the hop and chain limits. `paused` holds every team message until Resume, in order;
   a running turn is not interrupted.
3. **How the daemon knows a task is merged.** *Chosen*: the task's head commit is an
   ancestor of the integration branch (`git merge-base --is-ancestor`), checked when a
   member's turn ends and when a client reads the team. No hosting service is called, so
   a merge made outside the app is noticed the same way. *Rejected*: pull request state
   from GitHub — it needs credentials and a network, and is not the branch that matters.
4. **Storage shape.** *Chosen*: three journal tables. `teams` keeps one JSON document
   per team (settings, members, tasks with criteria and history, decisions, the id
   counters) with its repository, revision and times; the id is
   `INTEGER PRIMARY KEY AUTOINCREMENT`, so it is never reused. `team_members` maps a
   session to its team with the session as primary key, so "at most one team" is a
   constraint of the database. `team_updates` is the append-only log with a digest
   mark. A team is small (seven members, tens of tasks) and always changes as a unit
   under one revision, so a document avoids six join tables and keeps every change
   atomic. Deleting a team deletes its three kinds of rows and no conversation.
   - A **deleted** conversation leaves its team: a developer as if removed (its open
     tasks become unassigned); for the leader or the reviewer the team is paused and a
     decision names the empty role.
   - **Archiving** a member is refused with `team_member`.
   - A **fork** of a member is not a member.
5. **The first plan.** *Chosen*: the leader's first list of tasks waits for the user as
   one decision, "Start these N tasks?". Assignments made before it is answered are
   recorded and held; "Start" releases them. Later re-plans are logged only, because
   only the first plan fans out to every developer at once.

## Protocol

Features `teams/1` and `teams.tasks/1` in `daemon.hello`. Requests, for trusted local
clients only (`forbidden` otherwise; agents use the tools below):
`team.list`, `team.get`, `team.create`, `team.update`, `team.delete`,
`team.member.add`, `team.member.join`, `team.member.update`, `team.member.remove`,
`team.task.list`, `team.task.get`, `team.task.create`, `team.task.update`,
`team.task.assign`, `team.task.handover`, `team.task.move`, `team.task.criteria.set`,
`team.decision.list`, `team.decision.answer`. Events `team.changed` (the team and its
revision) and `team.removed` go to every local client.

`team.create` takes `execution_policy` (`read-only`, `ask` or `full`; `ask` when absent):
what the leader and the developers may do without asking. `full` needs
`risk_acknowledged`, as it does for one conversation (`execution_policy_denied` otherwise). It is the execution policy of
those conversations when the team starts them, at creation and for a developer added
later; the user changes one member in its conversation. The reviewer is not in it: its
review turns are read-only whatever its conversation's policy is (ADR 0067).

Errors: `role_taken`, `role_required`, `team_full`, `last_developer`,
`already_member`, `other_repository`, `reviewer_unsupported`, `lane_overlap`,
`team_member`, `team_not_found`, `task_not_found`, `invalid_transition`,
`not_owner`, `role_forbidden`, `dependencies_open`, `uncommitted_work`,
`decision_closed`, `invalid_payload`.

Agent tools in the ketplus_agents server, served only to members: `team_get`,
`team_tasks`, `team_task_update` (developer, own task), `team_task_create`,
`team_task_assign`, `team_task_handover`, `team_task_criteria`, `team_lane_set`,
`team_decision_raise` and `team_decision_withdraw` (leader). A call by the wrong role names
the role that may.

## Consequences

- The daemon, not the prompt, enforces every role rule; role instructions only explain.
- The phone does not show teams yet; the cloud projection is unchanged.
- An older client without `teams/1` shows the members as ordinary conversations, and
  unknown message kinds (`assignment`, `digest`, `decision`) as plain text.

## Alternatives considered

- **Teams in the desktop only.** Rejected: a team must survive a restart and every
  client must see the same one; the conversations it coordinates live in the daemon.
- **Status by agent messages.** Rejected: it is what a team replaces; every status
  message started a turn somewhere.
