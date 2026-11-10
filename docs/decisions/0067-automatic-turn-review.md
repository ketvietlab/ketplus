# 0067: Automatic turn review by a chosen reviewer conversation

Status: accepted (2026-11-08). Implements #120; the header picker is part of #159.

See also: ADR 0112 (agent teams) reuses this engine with a second trigger: a team's
reviewer reviews each task when it enters In review, and a member's own setting pauses.

## Context

Users already pair conversations by hand: a working agent finishes a turn, the user
asks another conversation to review it, and pastes the findings back. ADR 0027 gives
conversations numbers and an `AgentBroker` that delivers messages, wakes idle
receivers, turns a final answer into a reply and limits automatic turns. #120 asks
KetPlus to run that loop itself, with a reviewer that can never change the
repository.

## Decision

### Assignment

A conversation may name one **reviewer conversation** on the same host. The daemon
stores it in `conversation_reviewers` (working session id, reviewer session id,
`auto_fix`, `max_rounds`, update time) in the local journal. Defaults:
`auto_fix = true`, `max_rounds = 2`. Valid values are 1–3, because ADR 0027 holds
messages past hop 6. A conversation cannot review itself, and the reviewer must
be a Claude conversation, the only provider that enforces the guard below on every
tool call. Deleting either conversation removes the row.

Authenticated local Desktop IPC reads and writes the assignment
(`session.reviewer.get`, `session.reviewer.set`), advertised in `daemon.hello`
capabilities. Agent-token and cloud-control connections cannot call it.

The Desktop picker offers **No reviewer**, **New review agent** and **an existing
Claude conversation**. A new review agent (`new_provider`: `claude`) is created
when the user chooses it: a read-only conversation in the same worktree, titled
"Reviewer for #N", whose first turn only states the role and replies "Ready.".

### Read-only review turns

Every turn the broker starts for a review request runs under a **review guard**,
whatever the reviewer conversation's own mode is. Claude runs it in
`permissionMode: plan`, and the sidecar denies every tool that asks for permission
instead of forwarding a prompt. That covers `ExitPlanMode`, edits, Bash and MCP
tools other than `ketplus_agents`. Read, search and the agents server stay
available. The user therefore cannot accidentally grant a reviewer write, push,
merge or approve access from a prompt.

Codex and Cursor are not reviewers. Codex 0.154 launches the browser MCP server
for the whole app-server process with `default_tools_approval_mode = "approve"`,
and `turn/start` has no per-turn control over MCP servers. A read-only sandbox
would therefore still leave browser actions available. Cursor forwards permission
prompts. Codex can return once a per-thread guard is verified with a live
provider. Because a conversation can hand off to another provider after it was
chosen, the broker checks the provider again when it asks for a review and when
it delivers one. A reviewer that no longer enforces the guard gets no review:
the request is skipped or fails with `reviewer_unsupported`.

The guard applies only to review requests, and a review request runs only in such
a turn. A queued review waits for the reviewer's next own turn rather than joining
a turn the user starts there, and a held one waits for an explicit release. The
reviewer's user-started turns keep its mode.

### Trigger and request

When a turn of a conversation with a reviewer ends with status `completed`, the
broker asks for exactly one review:

- Cancelled, failed or stopped turns, and conversations without a reviewer,
  trigger nothing. Their changes stay in the next review's diff.
- A review is skipped with a `review_skipped` notice in the working conversation
  when the reviewer no longer exists (`reviewer_missing`), no longer enforces the
  guard (`reviewer_unsupported`) or is busy (`reviewer_busy`), or when the chain
  already used `max_rounds` reviews (`round_limit`).
- The turn's changes are the difference between two fixed snapshots of the
  worktree. A snapshot is the tree of every tracked and untracked, not ignored
  file. It is written by `git add -A` and `git write-tree` into a copy of the
  worktree's index, so the worktree and its index stay untouched. That includes
  edits to untracked files that already existed. One snapshot is taken when the
  reviewer is assigned and one after each completed turn, with git run off the
  event loop. A newer snapshot of the same conversation always wins over an older
  one that finishes later.
- A turn whose tree equals the baseline asks for no review and leaves a
  `no_changes` notice. If git fails, the notice says `snapshot_failed` and the
  baseline stays, so the next turn's review still covers these changes.
  Snapshots live in memory, so after a daemon restart the first comparison is
  with `HEAD`.

The request is an ordinary ADR 0027 message of kind `review` with `expect_reply`.
It names the working conversation's number and worktree, the base snapshot, the
user's request for the chain, the changed files and the diff. It also asks for a
structured answer ending in `Verdict: no findings` or `Verdict: findings`. The
whole request stays within the 32 KiB message limit: the file list is capped, the
diff keeps whole files while they fit, and a note names the file that was cut and
every file left out.

### Reply and loop

The reviewer's answer becomes the reply as in ADR 0027 and appears in the working
conversation attributed to `@agent/<reviewer number>`. Whether it arrives as the
final answer or through `agents_send` with `reply_to`, the broker reads the
verdict line:

- `findings`, `auto_fix` on and rounds remaining: the reply is delivered normally,
  so the idle working agent starts a turn to address it. That turn's completion
  asks for the next round.
- `no findings`, `auto_fix` off, or the last round: the reply is recorded with
  delivery `review_record` and shown in the working transcript as an
  `agent_message.state` notice (`review_recorded`, with its verdict and body),
  without starting a turn.

A missing verdict line counts as findings, so the round limit still ends the loop.
Turns started by a review request are never reviewed themselves, even when two
conversations review each other.

ADR 0027's hop, chain and hourly limits still apply, and the user can stop a
running turn at any time.

## Consequences

- Review turns spend the reviewer provider's quota without a user prompt, bounded
  by `max_rounds` and the ADR 0027 limits. Quota-aware skipping is not part of
  this change.
- The diff is fixed when the turn ends; the reviewer reads the same worktree,
  which may have moved on if the user starts another turn meanwhile.
- Snapshots write blobs for untracked files into the repository's object store,
  as `git stash create` does for tracked ones. A very large untracked tree that
  git cannot snapshot within 10 seconds is reported as `snapshot_failed`.
- Tests use the scripted fake provider and never call a real provider.
