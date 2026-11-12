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
  The repository is the main worktree git names for a path. The daemon asks git for it
  only where a repository is named or a conversation is to join a team (`team.list`,
  `team.create`, `team.member.add`), off its event loop before the command runs, and
  keeps git's answer. A path it was not asked about beforehand is still asked on the event
  loop, the first time.
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
instead of skipping them. The daemon reads the diff off its event loop before the review
starts and stores nothing until it has it: the task stays queued meanwhile, a restart asks
again, and a diff that is no longer for the next task or the team's integration branch is
read again. The reviewer is therefore a Claude conversation, the only
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
are reverted or covered. The files changed are the diff from where the branch left the
integration branch; a team without one counts only the branch's own commits since the
task's base (first parent, no merges), so merging the main line into the branch flags
nothing. A team's document reaches clients in one 1 MiB frame, so a task keeps at most
200 outside paths, and a history entry at most 2,000 characters of text and 50 items per
list, with the full count as `<key>_total`; documents written before the bounds are
bounded when read. *Rejected*: asking the user for every lane change — it is
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
| a member's work left open, or the whole team idle with work left (§8) | that member (`nudge`), then the leader (`stall`) |
| anything else | nobody |

Actionable updates: a task became `ready`, `blocked` or `done`, a review hit its round
limit, a path was flagged outside a lane, a developer has no task left, a developer's
question. When the leader's turn ends with actionable updates waiting, or after a quiet
period of **20 s** while it is idle, the daemon sends one digest of every update since
the last one, grouped by task. Other updates ride along in the next digest.

### 8. A team that stops is moved on (decided 2026-11-11)

The table above is event-driven: a turn that ends with work left and no event to follow
leaves the team standing. It happened in the first real team: a developer's turn ended
with its task in progress, the leader ended its turn to wait for work outside the team,
and nothing woke either of them.

The daemon therefore watches each team. **3 minutes** after a member's turn ends it
looks at a team that is not paused, has no open decision and has a task that is not done
(or no task yet). A team no member of which ends a turn is not looked at, so starting the
daemon does not wake teams that were left long ago:

1. An idle developer that owns a task `in_progress`, and an idle reviewer with a task
   `in_review`, gets one `nudge`: go on, or block the task with what you wait for. Once
   per member until the team's work changes; other members working does not delay it.
2. When nobody works and nobody was just nudged, the leader gets a `stall` message that
   lists every open task with its owner, at most **twice** while the work does not change.
3. After that the daemon raises a `stalled` decision on the team page: "Wake the leader
   again" (recommended) or "Pause the team". An open decision stops the watch, so the
   team now waits for the user visibly instead of silently.

"The work changed" means a task's state, owner, met criteria, pull request or head
commit, or the number of decisions. Nudges and stall wakes are automatic turns and count
against the budget (open point 2). The count is kept in memory; a restart begins it again.

The user's Stop is respected: a member whose turn the user stopped is not nudged, and a
leader the user stopped is not woken, so step 3 follows at once. A member's next turn
that ends by itself lifts that, and "Wake the leader again" lifts it for the leader only.
A worker that exits without a new turn changes nothing.

**Waiting for something outside the team.** A leader or developer that waits for a
build, a deployment or another conversation calls `team_wait` with what it waits for and
1 to 60 minutes, then ends its turn. The wait is an entry in Updates. Until it ends the
watchdog treats that member as at work: it is not nudged and the leader is not woken on
its account. When it ends an idle member is woken to check (message kind `wait`), an
automatic turn counted against the budget and held while the team is paused; a member
that already works again is left alone. A member has one wait, a new one replaces it; the
user's Stop, leaving the team and deleting the team end it, and the Stop also drops a
wake that is held because the wait ended while the team was paused. Resume asks again:
a held `wait`, `message_failed`, `nudge` or `stall` is not delivered to a member that
is stopped at that moment. Waits are kept in memory: a
daemon restart loses them and the watchdog takes over. The reviewer does not wait.

**A message outside the team that failed.** When a member's message to a conversation
that is not on its team fails, expires or gets no answer, the broker's note in the
transcript is not read by the agent, whose turn has ended. The member is woken with what
happened (message kind `message_failed`) and decides: send again, go on, block the task,
or, for the leader, raise a decision. A member the user stopped is not woken. A turn the
provider refuses to start ends with `runtime.error` alone, so the host marks that event
`turn_start_failed` with the turn's `turn_sequence`, and the broker ends the requests
that turn carried as `turn_failed` instead of leaving them to expire. A `runtime.error`
without the mark, or of another turn, ends nothing: an error inside a turn that goes on
may be journaled after the turn has answered.

## Open points, fixed here

1. **A developer's working directory.** *Chosen*: a task owns its worktree. When a task
   is first assigned the daemon creates branch `team/<team-id>/t-<n>` from the head
   of the integration branch, in a worktree beside the repository as ADR 0111 places
   them (`<repository>-team-<team-id>-t-<n>`), and
   re-points the owner's conversation at it with the worktree switch of ADR 0111 before
   the assignment turn starts. A handover re-points the new owner. The previous owner leaves
   the task's worktree only when its conversation stands in it: it goes to the task it
   works on, else to another task of its own, else back to the repository; a conversation
   that stands elsewhere is left alone. The move is stored with the handover
   (`relocations`). A conversation that cannot move during its turn leaves when the turn
   ends or before the team's next message to it, unless the task came back to it. The
   daemon makes the worktree off its event loop, and reads there what a hand to review
   needs (the worktree's status and the branch's commit). The command that asks for them
   (the user's `team.task.assign`, `team.task.handover` and `team.task.move`, a member's
   `team_task_assign`, `team_task_handover` and `team_task_update`) is answered when git
   has answered, and is checked then against the team as it has become. A command that
   would be refused asks git nothing, and one worktree is asked for a task however many
   commands wait for it. A worktree made for a command that was refused meanwhile stays
   with the task, because its path and its branch are taken. The worktree switch of
   ADR 0111 that follows still lists the repository's worktrees on the event loop, as the
   user's own switch does. *Rejected*: one
   worktree per developer switching branches — a handover would have to move
   uncommitted work between worktrees, and a developer could carry it into the wrong
   task.
2. **The automatic-turn budget.** *Chosen*: a team may start a **budget of automatic
   turns** of its members since the user last acted on it (created, resumed or answered a
   decision). When it runs out, the team pauses itself and raises one decision:
   "Continue (N more turns)" or "Stop", N being the team's own budget.
   - *Where the number comes from* (2026-11-12): the daemon keeps one global value,
     **40** until the user changes it in Settings → Teams (`team.settings.get` and
     `team.settings.update`, feature `teams.settings/1`, kept in the journal's
     `team_settings` table). A team created without `turn_budget` (an older phone, an
     agent, a client that sends none) takes it; New team shows it and lets the user send
     the team's own. Changing the global value changes no existing team.
   - *A running team*: `team.update {turn_budget}` (also from a paired phone) changes the
     team's own budget. It applies from the next automatic turn and the turns already used
     stay counted. A budget that leaves turns withdraws the open "used its N automatic
     turns" decision and resumes a team the budget paused; one that leaves none changes the
     number and keeps the pause. A team the user paused stays paused. The change is an
     Update of kind `turn_budget` (`data: {from, to}`).
   - *Range*: a whole number from **5 to 200**, the same for the global value, for
     `team.create` and for `team.update`. Anything else (4, 201, 7.5, a string) is refused
     with `invalid_payload` and "Automatic turns are a whole number from 5 to 200.";
     the desktop says the same words in the form, the team's dialog and Settings before
     sending. 5 keeps a team able to do a round of work; 200 keeps a forgotten team from
     running for days unattended. A value outside the range found in the journal counts as
     none (40).
   The loop guard of ADR 0027 (hop 6, 30 turns a
   chain, 20 an hour per conversation) applies to every automatic turn on top. Team
   messages start their own chain, so an `agents_send` loop inside a team still stops at
   the hop and chain limits. `paused` holds every team message until Resume, in order;
   a running turn is not interrupted.
3. **How the daemon knows a task is merged.** *Chosen*: the task's head commit is an
   ancestor of the integration branch (`git merge-base --is-ancestor`), checked when a
   member's turn ends and when a client reads the team. No hosting service is called, so
   a merge made outside the app is noticed the same way. The daemon asks git off its event
   loop (as it does for the lane check of §4): the read that asked is answered with the team
   as it is, and the merge follows as a `team.changed` event. One question is open for a team
   at a time, and its answer is applied only to a task that is still as it was asked about.
   An answer about an integration branch the team no longer has is dropped and asked again. The same check compares the
   branch with the commit that was reviewed: a ready task whose branch moved goes back to
   Blocked to be reviewed again, and one merged with commits after its review is done
   with a `merged_unreviewed` update to the leader. *Rejected*: pull request state
   from GitHub — it needs credentials and a network, and is not the branch that matters.
   *Amended by [ADR 0118](0118-team-tasks-done-by-pull-request.md)*: a task that records a
   pull request is done when the remote says it is merged, from any open state; the
   ancestor check counts only a branch with commits of its own and also accepts
   `origin/<integration>` after a fetch; the leader can close a task with
   `team_task_close`.
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
   recorded and held; "Start" releases them. Any other answer goes to the leader, and
   the question is raised again when the leader's turn ends, until it is "Start". Later re-plans are logged only, because
   only the first plan fans out to every developer at once.

## Protocol

Features `teams/1`, `teams.tasks/1` and `teams.settings/1` in `daemon.hello`. Requests, for
trusted local clients only (`forbidden` otherwise; agents use the tools below):
`team.settings.get`, `team.settings.update` (`{turn_budget}`; both answer
`{settings: {turn_budget, turn_budget_minimum, turn_budget_maximum, turn_budget_default}}`),
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
`team_member`, `not_member`, `team_not_found`, `teams_unavailable` (the journal could not be read, so no
team request is served; ask again), `task_not_found`, `invalid_transition`,
`not_owner`, `role_forbidden`, `dependencies_open`, `dependency_cycle`,
`uncommitted_work`,
`decision_closed`, `invalid_payload`.
A team's integration branch is a name git accepts (`git check-ref-format --branch`) and
never one that reads as an option; a team that has one changes it, not drops it
(`invalid_payload`). Every branch the daemon hands to git follows `--end-of-options`, and
paths are read with `-z`, so a path is spelled as it is.

Agent tools in the ketplus_agents server, served only to members: `team_get`,
`team_tasks`, `team_task_update` (developer, own task), `team_wait` (leader and
developers), `team_task_create`,
`team_task_assign`, `team_task_handover`, `team_task_criteria`, `team_task_close`
(ADR 0118), `team_lane_set`,
`team_decision_raise` and `team_decision_withdraw` (leader). A call by the wrong role names
the role that may.

## Consequences

- The daemon, not the prompt, enforces every role rule; role instructions only explain.
- A paired phone reads and changes teams as ADR 0116 says: the daemon advertises
  `teams.remote/1`, answers sealed `team.*` mailbox commands through the same
  `TeamCoordinator` rules, and adds a `team` field to the sealed session metadata.
- An older client without `teams/1` shows the members as ordinary conversations, and
  unknown message kinds (`assignment`, `digest`, `decision`) as plain text.

## Alternatives considered

- **Teams in the desktop only.** Rejected: a team must survive a restart and every
  client must see the same one; the conversations it coordinates live in the daemon.
- **Status by agent messages.** Rejected: it is what a team replaces; every status
  message started a turn somewhere.
