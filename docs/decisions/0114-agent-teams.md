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
- Exactly **one leader**, **one to five developers** and **one or two reviewers**
  (T-23).
  A conversation is on at most one team. Members belong to the team's repository.
  The repository is the main worktree git names for a path. The daemon asks git for it
  only where a repository is named or a conversation is to join a team (`team.list`,
  `team.create`, `team.member.add`), off its event loop before the command runs, and
  keeps git's answer. A path it was not asked about beforehand is still asked on the event
  loop, the first time.
  *Rejected*: more than one leader, since two leaders split the plan, and more than two
  reviewers. A second reviewer was rejected here at first, because "it only hides a
  queue that a second team removes". The user reversed that on 2026-11-12: a second
  reviewer shortens the queue of one team. Each task is still reviewed by one reviewer,
  never by both (§2). New team offers one or two reviewers. In the team's settings
  (`team.member.add` with `role: "reviewer"`, `team.member.remove`) a second one is added,
  and either one is removed while the other stays. A third reviewer is refused with
  `role_taken`, and removing the last one with `last_reviewer`. A paired phone's New team
  keeps one reviewer. On the desktop, the Team page's queue banner offers "Add a second
  reviewer" while a team with three or more developers has one, and each reviewer's row
  shows its own review and how many tasks wait for it (`reviews`).
- The **leader** is the only member that reports to the user. It breaks the goal into
  tasks, writes their acceptance criteria (unless `leaderWritesRequirements` is off),
  assigns them, changes lanes and orders the merges. It neither writes code nor
  reviews.
- A **developer** owns a lane: a name and path prefixes.
- A **reviewer** reviews each task against its criteria. It owns no task, so it never
  reviews its own work, and it never merges.
- **No new authority**: nothing a member sends approves a permission request, answers a
  decision or merges a protected branch. ADR 0027 is unchanged; members may address
  `leader` and `reviewer` by role as well as by number. With two reviewers,
  `reviewer` names the one that reviews the sender's tasks. When that is not one
  reviewer, the send is refused with `ambiguous_role`, which names both numbers. The
  team's description (`team_get`'s `you.reviewers`) lists both.

### 2. The reviewer reuses ADR 0067 and reviews per task (decided)

One engine, a second trigger. A task entering `in_review` sends the team's reviewer a
`review` message with the task's criteria and the diff of the task's branch against the
integration branch it was cut from. The verdict line of ADR 0067 decides: `no findings`
makes the task `ready`; `findings` returns it to its owner with round + 1. Past
`max_rounds` the task is `blocked` and the leader's digest says why (see *Review rounds
and where a team reviews* below).
A reply without the line has **no verdict** (T-19): it does not count a round and goes
to no one but the reviewer, which is asked once more as the same round. A second reply
without a verdict blocks the task with one actionable update for the leader. The
history records each such reply as a `verdict` entry with `no_verdict`. The leader
corrects the record with two tools, refused with `role_forbidden` for any other role:
`team_review_void` (task and reason) voids the last counted round of a task that is
`ready`, `in_progress` or blocked by its review, and returns it to `in_review` with the
round back by one and its open merge decisions withdrawn; `team_review_accept` (task,
evidence such as a pull request review's URL, and reason) makes a task in review, or
blocked by its review, `ready` and drops a queued or running review of it. Both leave
history (`verdict_voided`, `review_accepted`) and need no new commit.

A busy reviewer **queues** requests first in, first out, instead of skipping them.
**Sharing requests (T-23).** Each reviewer has its own review in flight and its own
queue, so two reviewers review two tasks at the same time. A task entering `in_review`
goes to exactly one of them:
1. the reviewer of its last round, while that reviewer is on the team;
2. otherwise the idle one;
3. otherwise the one with the shorter queue (the first to join on a tie).

When a reviewer leaves (removed or deleted), the other one takes its queue. The review
it had not finished goes first, as the same round: it is not counted again. A team
without a reviewer keeps its tasks waiting, and the next reviewer takes them. The
team's document stores this as `reviews` (`reviewer`, `reviewing`, `reviewing_message`,
`queue`) and `review_waiting`. It still writes `reviewing`, `reviewing_message` and
`review_queue` for a reader from before: the first review in flight and every waiting
task. The daemon reads the diff off its event loop before the review
starts and stores nothing until it has it: the task stays queued meanwhile, a restart asks
again, and a diff that is no longer for the next task or the team's integration branch is
read again. The reviewer is therefore a Claude conversation, the only
provider that enforces the review guard. A team's review round runs under the guard's
**verify** profile (ADR 0067): the reviewer may build, run tests and use the browser
without a prompt and write only in its own scratch directory; it cannot change a
tracked file, commit, push, merge, approve or close a pull request, delete a branch or
worktree, or change its own permissions. No permission prompt is forwarded from a
review turn. The reviewer's other turns (its clean-up, the leader's messages) keep its
own mode, as before. A member's own Reviewer setting stays
stored but does not run while it is on a team, and `session.reviewer.set` is refused
with `team_member`. *Rejected*: a second review engine — it would drift from 0067.

**Review rounds and where a team reviews (T-40, decided 2026-11-12).** Both are team
settings, chosen in New team and changed in the team's Review settings (`team.create` and
`team.update` take `max_rounds` and `review_place`):

- `max_rounds` is **1 to 5**, default 2 (T-53, 2026-11-12: the user asked for "Max
  review passes per PR" as a team setting; it was 1 or 2 under T-40). Any other value
  is refused with `invalid_payload` ("A team reviews a task in one to five passes
  before the leader decides."). A stored team with a value outside 1 to 5 reads as
  two. A change is logged in Updates as "Max review passes: N". With N passes, a task
  whose pass N ends with findings is `blocked` for the leader with the `round_limit`
  update.
- A lowered limit applies from the **next review request**, never retroactively: a
  review in flight keeps its limit, and a task that already had as many passes as the
  team now allows is `blocked` with an actionable `round_limit` update when it next
  enters In review, or when a reviewer takes it from the queue if the limit was
  lowered while it waited, instead of being reviewed. Its limit then becomes the
  team's, so a leader who hands it to review again gets that pass. A raised limit gives
  a task its new passes at its next request. Voiding a pass (`team_review_void`) after
  the limit was lowered hands the task back to In review, so the same check applies:
  it is blocked, and the leader hands it to review once more to get the voided pass
  again. A team stored with `max_rounds: 3` before T-53, when three was out of range
  and read as two, now reads as three.
- `review_place` is `board` (default) or `pull_request`; anything else is refused in
  words.
- A task takes both when it **enters a round** (`round_limit`, `review_place` on the
  task), so a change applies to tasks that enter review afterwards; a round in flight
  keeps what it started with. The place is `board` for a task without a pull request
  URL, and the review message and the history say so ("Reviewed on the board: the task
  has no pull request").

On the pull request, the reviewer reviews the same diff under the same verify guard and
replies as on the board, with the verdict line; the message tells it that its reply is
posted as a review comment on the task's pull request and not to post it itself. The
guard keeps refusing `gh pr comment` and `gh pr review`: the reviewer stays read-only,
and **the daemon posts the reply** with `gh pr comment <url> --body-file -` on the same
bounded worker as the pull request probe (ADR 0118), headed with the task id and the
round only. The pull request may be public, so the reviewer is told to write the comment
in English and name no folder of this computer and no person, and the daemon replaces
the task's worktree, the team's repository and the home folder in it (`<task worktree>`,
`<repository>`, `~`) before posting; no member's name or task title is in the header. A
round past a limit lowered since the task's last round shows as its own last round
("round 2 of 2"), never "2 of 1". *Rejected*: letting the guard allow a comment — it would widen a security
property of ADR 0067 for one provider, and the daemon reaches the same comment for every
reviewer. While the comment is posted the review stays bound to its request, so a stale
or repeated answer is ignored and the reviewer takes no other review. Then the board
follows it as it follows a review on the board:

- the round is counted; the `verdict` entry keeps the verdict, the round,
  `place: pull_request` and `comment_url`, not the findings' text;
- findings return the task to its owner with the comment's link and the findings;
- the round limit blocks it for the leader, the question naming the link;
- `team_review_accept` on such a task names the last comment's link in its history
  (`comment_url`) beside the leader's evidence.

A comment that cannot be posted (no `gh`, not signed in, offline) does not lose the
review: the verdict is recorded on the board with the findings' text, `comment_error`
and the reason in words. A reply without a verdict is never posted. A restart while a
comment is posted leaves the review bound with no answer coming; the leader voids or
accepts it as any stalled review.

### 3. Decisions live on the team page (decided)

A question that changes the product is one **decision** on the team: question, context,
two to four choices, the leader's recommendation, the task it blocks. Only the leader
raises one; the daemon raises its own (the first plan, a used-up budget, a merge that
waits for the user, an empty role). The answer is the user's, sent by a client, and is
delivered once to the leader and to the owner of the blocked task as a `decision`
message. The team page shows it first; the team's folder shows the count; the leader's
row shows the "Waiting for you" status of #125. *Rejected*: an app-wide inbox — a
second place to look for the same question.

*Amended (T-55, 2026-11-13)*: the card's last choice is **Other…**, the user's own words.
It opens a multi-line field on the card: Enter sends, Shift+Enter adds a line, Esc or
Cancel puts the card back, and Send waits while the text is blank. The text is the
`choice` of `team.decision.answer`, which the daemon trims to 4,000 characters and
refuses when empty. A refusal (`decision_closed`, `invalid_payload`) shows on the card
and keeps what was typed, because the page keeps the text across its rebuilds. The answer
goes to Updates as "D-N answered: <text>", as a choice does. The card no longer has
"Answer in the conversation"; a decision is answered on its card. The phone sends choices only
for now; the phone app tracks a written answer as a follow-up.

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

*Amended (T-38)*: only tracked changes are compared. The committed part is read from the
task's branch (`refs/heads/<branch>`), never from whatever the worktree has checked out; the
uncommitted part is the tracked files changed in the worktree (`git status -uno`), and only
while the worktree has the task's branch checked out. Untracked and ignored files, such as
build trees and screen captures, are never outside a lane. Paths any lane may change, such
as contracts, snapshots, fixtures and tests, are named by the leader with
`team_paths_allow {paths, reason, task_id?}`: without `task_id` they are the team's shared
paths, with it the task's own `allowed_paths` (sent with the task only when set). They are
validated as lane paths (relative, inside the repository, `invalid_payload` otherwise), the
list replaces the one before, the change goes to Updates and, for a task, to its history,
and every flag they now cover clears. The team page shows the shared paths with each
lane's paths and the task dialog a task's allowed paths.

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
| any review verdict | the leader, in its next `digest` (T-75) |
| decision answered | the leader and the blocked owner (`decision`) |
| actionable updates | the leader, once, as a `digest` |
| a member's work left open, or the whole team idle with work left (§8) | that member (`nudge`), then the leader (`stall`) |
| anything else | nobody |

Actionable updates: a task became `ready`, `blocked` or `done`, a review verdict, a
review hit its round limit, a path was flagged outside a lane, a developer has no task
left, a developer's question. When the leader's turn ends with actionable updates waiting, or after a quiet
period of **20 s** while it is idle, the daemon sends one digest of every update since
the last one, grouped by task. Other updates ride along in the next digest.

**Every verdict reaches the leader (T-75, decided 2026-11-13).** A findings verdict sends
the task back to its developer, so before T-75 it moved no task to an actionable state, and
the leader heard of it only with a later actionable update or its own wait. Now each
verdict adds one actionable `verdict` update. That makes the leader's next digest due
after the quiet period, or when its turn ends if it is busy. The update covers findings,
no findings, and findings at the round limit. Its line names the task, the pull request
when the task has one, the round of its limit, the reviewer and the verdict. For findings
it adds the count by severity:

- `Verdict on T-61 (PR https://…/pull/12), round 1 of 2 by Reviewer: findings (0 High,
  0 Medium, 6 Low, 3 Nit). Back to its developer; decide on any finding outside the
  task's scope.`
- `… round 2 of 2 by Reviewer: no findings. Ready to merge.`
- `… findings (1 High, …). The round limit is reached: you decide.`

The severities come from the review reply. A finding is a line that starts with its id
and severity, in the forms the team's reviewers write (`- **H1, High.** …`,
`- **L1 (Low), file:4:** …`, `**M1 — Medium — …**`), or with its severity alone
(``- **Low**, `file:4`: …``). An id is counted once, so a finding mentioned again in the
prose is not counted twice. A finding marked as settled is not counted, as round 2 lists
round 1's findings: ✅ or "đã sửa" anywhere on its line settles it
(`- **L1 (Low), …:** ✅ **đã sửa.**`), and so does "fixed" where it reads as the mark (at
the end, or before "in", punctuation or a dash: `L1 — Low — fixed in abc12`), not in a
finding's own words ("fixed findings count"). A line also marked ❌ is open. A severity
followed by a count (`- High: 0`) or by nothing (`none`, `không có`) is no finding. A reply
without such lines counts each line that names a severity before a colon, but not a
summary of counts (`High: 0, Medium: 1`). A reply that names no severity reads "severity not stated". At the
round limit the verdict line is the `round_limit` warning itself, so the leader reads one
line besides the task's blocked state. The same line is in the team page's Updates and in
the phone's team payload, which both show the team's updates. No message from member to member is added. The developer still
receives the findings as the `review` reply, and the leader reads only the line.

**A wait alone sends no digest (decided 2026-11-12).** A `wait` entry is shown on the
team page and in a digest that has something else in it, but it is never the reason for
one: when the only undigested updates are waits, the leader's own or a developer's, no
digest is sent and no turn starts. The waits stay undigested and ride along with the
next digest that has an actionable update. A member's new wait replaces its earlier
one, so a digest lists only the latest wait of each member; a leader that renews its wait
does not read it back several times. The wake-up when a wait's minutes are over (§8) is
unchanged.

**A command is answered before what it sets off (decided 2026-11-12).** A wake is
decided when its cause is committed: the budget check, a hold while paused and the turn
it counts. The message, and for an assignment the worktree its owner is pointed at, go
into the team's stored `outbox` in that same commit. The command is answered next. The
daemon then sends the outbox one entry per event-loop pass, oldest first:

1. It points the receiver at its worktree, when the entry names one. A pending move of the
   same receiver out of a handed-over worktree is settled by that switch once it worked.
   An entry without a worktree, or one whose switch failed, settles such moves first.
2. It queues the message.
3. It forgets the entry.

Pointing a conversation at a worktree asks git. With two released assignments, that took
a first-plan Start 4 s with a git that answers in 2 s, and seconds on a busy machine. So
the checks that need no git run on the daemon's thread, git runs on the team's git
thread, and the switch is checked again and stored on the daemon's thread when git
answers. The entry stays stored meanwhile, and the next entry waits for it, so the order
is kept. A turn start likewise remembers its worktree's repository with git off the
daemon's thread. Each entry carries the id its message gets, and an entry whose message
is already queued is only forgotten. So a daemon that stops between a reply and its
sends sends what is left on its next start, once. A pause answered after a Start does
not hold what that Start released. Clients never see the `outbox`: the team JSON leaves
it out.

Settling a move out of a handed-over worktree also runs off the daemon's thread
(T-32), wherever it happens: after a handover is answered, in an outbox entry, when the
member's turn ends, and on the next start for moves stored before a stop. One
conversation's switches run one after another: each decides where the conversation goes
once the switch before it is stored, and the outbox's message waits for them. A switch
that worked forgets its move. One that failed, also because the worktree changed while
git ran, keeps it and is tried again when the member's turn ends; after a handover the
reason is also noted on the task. A command that names a member standing in a task's
worktree finds that worktree's repository from the team, without git, so a handover is
answered before any git runs.

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

**An urgent message to a member that cannot be steered (decided 2026-11-12).** A
member's turn may be over while a command it started in the background still runs. A
message sent to it with `urgent` cannot steer that turn. The sender's receipt then says
`urgent_waiting` instead of `queued`, with `waiting_on` (for example "a background task")
and a note that it could not steer yet. The message steers the receiver's turn at the
first point its provider allows, or starts a turn once the receiver is idle. ADR 0027
lists every receipt state: `starting`, `queued`, `steering`, `urgent_waiting` and
`recorded`.

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

### 9. The goal is long and may mention conversations (decided 2026-11-12)

The goal is the leader's main input, so it is written like a brief, not a title.

- **Length.** A goal keeps its line breaks and holds up to 16,000 characters
  (`teams::rules::maximumGoalCharacters`) and up to 24 KiB of UTF-8
  (`maximumGoalBytes`). The leader's first turn is one team message, which the broker
  holds up to 32 KiB of UTF-8, so the rest of that turn fits beside the goal. Accented
  letters, other scripts and emoji take more than one byte each, so such a goal reaches
  the byte limit first. The daemon refuses a longer goal on `team.create` and
  `team.update` with `invalid_payload`, naming the count. Nothing cuts a goal: the
  leader's first turn, the journal and the remote summary carry it whole. New team shows
  the character count under the field and says, with the count, which limit a goal is
  over before Create.
- **Mentions.** `@agent/<number>` in the goal names a conversation on this computer
  that holds context, as in the composer. A mention means context only:
  - Creating the team messages nobody and adds nobody to it.
  - The leader's first turn lists the mentioned conversations, each with its number and
    title, or says that no conversation has that number. It lists the first 20, with
    titles cut at 80 characters, and counts the rest, so the turn stays one message. It tells the leader that it
    may ask them through agent messaging (`agents_send`).
  - A mention starts at the start of the goal, after a space, or after an opening
    bracket or quote, so an e-mail address is not one.
- **The field.** New team uses the kit's multi-line field (`ket::TextArea`). It shows
  six lines, grows to fourteen, then scrolls. Return starts a new line, and
  Cmd/Ctrl+Return creates the team. Typing `@` opens the composer's conversation picker
  and inserts the mention as one token. A number no conversation has is marked, and the
  form names it before the team can start.
- **Where it shows.** The team page shows the goal under its name, folded after three
  lines with Show more, and a mention there opens its conversation. Every other place
  (the sidebar folder's tooltip) shows its first line, cut at the end. The desktop app
  has no place to change a goal after the team exists. A place added later uses the
  same field.

### 10. Disk space and the worktrees of done tasks (decided 2026-11-12)

Every task has its own worktree, and a done task's worktree stays on disk until someone
removes it. A team of a few developers fills a disk quickly, so the team page says how much
space is left and offers the worktrees of done tasks for removal. It only offers: nothing
is paused, stopped or removed on its own at any level.

- **The disk.** The daemon measures the free space of the volume that holds the team's
  repository (where its worktrees are made, open point 1) with `QStorageInfo`, off the
  event loop. Two levels, constants in `teams::storage` (`TeamStorage.h`), in decimal
  gigabytes as the Finder counts them:
  - below `warnBytes`, 20 GB: *low*. A warning banner names the free space and suggests
    removing the worktrees of done tasks.
  - below `stopBytes`, 12 GB: *stop*. A danger banner names the free space and says
    builds should stop until space is freed.
  When the space cannot be read, the page shows no banner.
- **The offer.** Each done task whose worktree is still a folder on disk, and is not the
  repository's main checkout, is listed under "Worktrees of done tasks" with its path and
  its size on disk (blocks used, links not followed). The page reads it when it opens,
  when the team's done tasks or their worktrees change, and every minute. A worktree is
  walked only the first time it is listed; the daemon keeps its size, so the reading every
  minute asks the volume alone.
- **Removal.** Only after the user confirms it in a dialog. The daemon refuses, in words
  and changing nothing, a task that is not done, a worktree that is not on disk, the main
  checkout (by its canonical path, or a folder with a `.git` directory), the worktree of a
  conversation that is running a turn or waits for an answer or an approval, a worktree with uncommitted or untracked changes
  (`git status --porcelain --untracked-files=all`), and one with commits on no remote
  branch (`git rev-list HEAD --not --remotes`). Otherwise it runs `git worktree remove
  <path>` without `--force`, then `git worktree prune`, both in the main checkout, in
  the order of the team's other git work. A failure is shown in git's words and the task
  keeps its worktree entry; a removal clears the entry and adds `worktree_removed` to
  the task's history. The task's branch and its commits stay. An idle conversation still
  in the folder (the developer of the team's last task stays in its worktree) is moved to
  the main checkout, so its next turn does not start in a folder that is gone. The history
  entry is written once every move has answered and names what happened to each one; a
  conversation that could not be moved is named with the reason, in the history and in
  the answer's `note`, which the dialog shows instead of closing.
- **Remove all (T-62, 2026-11-13).** The section's header has "Remove all…" while at
  least one worktree is listed. One confirmation names how many worktrees and their total
  size ("at least" when a size is unknown); nothing is removed before it. The page then
  sends `team.worktree.remove` for each listed worktree, one after the other answered, in
  the list's order, so each goes through the same guards and the same git order as a
  single Remove; there is no bulk request in the daemon. A refusal or a failure keeps that
  worktree listed with its reason in the daemon's or git's words, and the others are still
  removed. The dialog and the page show a summary (removed N, kept M with their reasons),
  each removal adds `worktree_removed` to its task's history as before, and "Remove all…"
  and every Remove… are disabled while the run lasts. The page's summary stays when the
  run removed every worktree (the section then says none is left on disk), and goes once
  a worktree it names as kept is no longer listed, such as after removing it by hand.
- **Only the user.** There is no agent tool for either request. Agent connections reach
  only `agents.*` operations, the requests are `team.*` and served to trusted local
  clients only, the remote team path (ADR 0116) does not carry them, and the coordinator
  refuses any actor but the user (`role_forbidden`).

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
   Inside a team the budget is the only limit on automatic turns. A turn that teammates'
   messages start (`agents_send` from one member to another) counts once against the
   budget, as a turn the team starts does; the loop guard of ADR 0027 (hop 6, 30 turns a
   chain, 20 an hour) does not apply to messages from the team or between its members. A
   member's message that finds the team paused or its budget used up is held as
   `team_paused` or `team_budget`; Continue or Resume delivers it, in order, together with
   messages an earlier build's loop guard held inside the team. A turn only the team
   started begins a new exchange, so what it sends outside the team is guarded from hop 1.
   Messages to or from conversations outside the team keep the loop guard. `paused` holds
   every team message until Resume, in order; a running turn is not interrupted.
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
   *Amended (T-38)*: a task gets a head commit only from a commit made on its own branch
   after its base. At a hand to review the daemon reads the branch; its tip is the task's
   head only when it differs from the base, has the base as an ancestor, is not contained
   in the integration branch and is not the head another task was handed in at. A worktree
   checked out at its base, at the integration branch or at another task's commit therefore
   gives the task no head, and its review gets no diff of another task: the diff runs from
   the integration branch to the commit handed in, and a branch still at its base sends
   none. When git cannot read the branch or its ancestry the hand to review is refused with
   `git_unavailable`, to be tried again. Without a head of its own it is refused with
   `no_commits`, unless
   the task records its pull request: work done in another worktree or repository is
   handed in with `team_task_update {state: in_review, pull_request, head_commit}`. The
   daemon records `head_commit` (7 to 40 hexadecimal characters, optional) as the task's
   head and a `handoff` history entry, and the review request names the pull request and
   that commit instead of a diff. Git's rule makes a task done only when its branch has
   commits of its own (its tip is not the base) and it was handed in at one of them; a
   branch at its base, an empty or unknown base, or a task with an open pull request is
   never done because the integration branch contains it.
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
   mark. A team is small (eight members, tens of tasks) and always changes as a unit
   under one revision, so a document avoids six join tables and keeps every change
   atomic. Deleting a team deletes its three kinds of rows and no conversation. Delete
   team… offers "Also delete its conversations", off by default and unavailable while a
   member runs a turn; with it the desktop deletes each member's conversation after
   `team.delete` succeeds, the same way as the conversation's own Delete, and keeps their
   worktrees.
   - A **deleted** conversation leaves its team: a developer as if removed (its open
     tasks become unassigned); for the leader or the last reviewer the team is paused and
     a decision names the empty role; one of two reviewers leaves as if removed.
   - **Archiving** a member is refused with `team_member`.
   - A **fork** of a member is not a member.
5. **The first plan.** *Chosen*: the leader's first list of tasks waits for the user as
   one decision, "Start these N tasks?". Assignments made before it is answered are
   recorded and held; "Start" releases them. Any other answer goes to the leader, and
   the question is raised again when the leader's turn ends, until it is "Start". Later re-plans are logged only, because
   only the first plan fans out to every developer at once.

## Protocol

Features `teams/1`, `teams.tasks/1`, `teams.settings/1` and `teams.storage/1` in
`daemon.hello`. Requests, for
trusted local clients only (`forbidden` otherwise; agents use the tools below):
`team.settings.get`, `team.settings.update` (`{turn_budget}`; both answer
`{settings: {turn_budget, turn_budget_minimum, turn_budget_maximum, turn_budget_default}}`),
`team.list`, `team.get`, `team.create`, `team.update`, `team.delete`,
`team.member.add`, `team.member.join`, `team.member.update`, `team.member.remove`,
`team.task.list`, `team.task.get`, `team.task.create`, `team.task.update`,
`team.task.assign`, `team.task.handover`, `team.task.move`, `team.task.criteria.set`,
`team.decision.list`, `team.decision.answer`, `team.storage.get` (`{team_id}`; answers
`{storage: {free_bytes, level, warn_bytes, stop_bytes, worktrees: [{task_id, title, branch,
path, size_bytes}]}}`, `free_bytes` and `size_bytes` null when unknown, `level` `low`,
`stop` or empty) and `team.worktree.remove` (`{team_id, task_id}`; answers `{team, note}`), §10.
The two are on the desktop wire only, not on the remote team path. Events `team.changed` (the team and its
revision) and `team.removed` go to every local client.

`team.create` takes `execution_policy` (`read-only`, `ask` or `full`; `ask` when absent):
what the leader and the developers may do without asking. `full` needs
`risk_acknowledged`, as it does for one conversation (`execution_policy_denied` otherwise). It is the execution policy of
those conversations when the team starts them, at creation and for a developer added
later; the user changes one member in its conversation. The reviewer is not in it: its
review turns run under the verify profile whatever its conversation's policy is (ADR 0067).

`team.create` and `team.update` take `max_rounds` (1 to 5; 2 when absent) and
`review_place` (`board` or `pull_request`; `board` when absent), and the team's document
returns both (§2). These are local commands; the relay contract and the phone's
`team.update` are unchanged, so a phone creates a team with two rounds on the board. The
phone's team summary carries `max_rounds` read-only (T-53, ADR 0116).

Errors: `role_taken`, `role_required`, `team_full`, `last_developer`,
`already_member`, `other_repository`, `reviewer_unsupported`, `lane_overlap`,
`team_member`, `not_member`, `team_not_found`, `teams_unavailable` (the journal could not be read, so no
team request is served; ask again), `task_not_found`, `invalid_transition`,
`not_owner`, `role_forbidden`, `dependencies_open`, `dependency_cycle`,
`uncommitted_work`,
`decision_closed`, `invalid_payload`, `worktree_not_removed` (§10, with the reason in
words).
A team's integration branch is a name git accepts (`git check-ref-format --branch`) and
never one that reads as an option; a team that has one changes it, not drops it
(`invalid_payload`). Every branch the daemon hands to git follows `--end-of-options`, and
paths are read with `-z`, so a path is spelled as it is.

Agent tools in the ketplus_agents server, served only to members: `team_get`,
`team_tasks`, `team_task_update` (developer, own task), `team_wait` (leader and
developers), `team_task_create`,
`team_task_assign`, `team_task_handover`, `team_task_criteria`, `team_task_close`
(ADR 0118), `team_lane_set`, `team_paths_allow`, `team_repository_set` (ADR 0123),
`team_decision_raise` and `team_decision_withdraw` (leader). `team_task_create` takes an
optional `repository` and `integration_branch` for a task that works in another repository
than the team's (ADR 0123). A call by the wrong role names
the role that may.

`team_tasks` (T-36) answers in shapes bounded for a model's context:

- No `task_id`: `tasks`, one row per task: `id`, `title`, `state`, `round`, `owner`
  (`{number, name}` or null), `lane`, `depends_on`, `pull_request`, and `question` when
  blocked. `open: true` leaves out done tasks; `mine: true` keeps the caller's own and
  reviewed tasks.
- `task_id`: the task with its description and criteria and its newest 10 history entries,
  or `history_limit` (1 to 50) entries from `history_start`, with `history_total` and
  `history_start`; each entry carries its index as `entry`.
- `task_id` and `entry`: `{id, entry, history_total}`, that entry uncut, including a text
  the document keeps cut (ADR 0115, whole texts).

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
