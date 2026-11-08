# 0067: Automatic turn review by a chosen reviewer conversation

Status: accepted (2026-11-08). Implements #120; the header picker is part of #159.

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
be a Claude or Codex conversation; Cursor cannot enforce the guard below. Deleting
either conversation removes the row.

Authenticated local Desktop IPC reads and writes the assignment
(`session.reviewer.get`, `session.reviewer.set`), advertised in `daemon.hello`
capabilities. Agent-token and cloud-control connections cannot call it.

The Desktop picker offers **No reviewer**, **New review agent** and **an existing
conversation**. A new review agent (`new_provider`: `claude` or `codex`) is created
when the user chooses it: a read-only conversation in the same worktree, titled
"Reviewer for #N", whose first turn only states the role and replies "Ready.".

### Read-only review turns

Every turn the broker starts for a review request runs under a **review guard**,
whatever the reviewer conversation's own mode is:

- Codex: `sandbox: readOnly` with `approvalPolicy: never`, so escalation requests
  are refused without a prompt.
- Claude: `permissionMode: plan`. The sidecar denies every tool that asks for
  permission instead of forwarding a prompt. That covers `ExitPlanMode`, edits,
  Bash and MCP tools other than `ketplus_agents`. Read, search and the agents
  server stay available.

The user therefore cannot accidentally grant a reviewer write, push, merge or
approve access from a prompt. Reviews of other conversations' turns are the only
turns this guard applies to; the reviewer's own user-started turns keep its mode.

### Trigger and request

When a turn of a conversation with a reviewer ends with status `completed`, the
broker asks for exactly one review:

- Cancelled, failed or stopped turns, and conversations without a reviewer,
  trigger nothing.
- A review is skipped with a notice in the working conversation when the reviewer
  no longer exists, is busy, or the chain already used `max_rounds` reviews.
- The turn's changes are the difference between two snapshots of the worktree.
  A snapshot is `git stash create` (else `HEAD`) plus the untracked files. It is
  taken when the reviewer is assigned and again after each finished turn, with
  git run off the event loop. A turn with no tracked change and no new untracked
  file asks for no review and leaves a `no_changes` notice. Snapshots live in
  memory, so after a daemon restart the first comparison is with `HEAD`.

The request is an ordinary ADR 0027 message of kind `review` with `expect_reply`.
It names the working conversation's number and worktree, the base revision, the
user's request for the chain, the changed files and the diff, truncated within the
32 KiB message limit with a note naming the omitted files. It also asks for a
structured answer ending in `Verdict: no findings` or `Verdict: findings`.

### Reply and loop

The reviewer's final answer becomes the reply as in ADR 0027 and appears in the
working conversation attributed to `@agent/<reviewer number>`. The broker reads
the verdict line:

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
- The diff is a snapshot taken when the turn ends; the reviewer reads the same
  worktree, which may have moved on if the user starts another turn meanwhile.
- Untracked files are listed but their contents are not inlined; the reviewer
  reads them from the worktree.
- Tests use the scripted fake provider and never call a real provider.
