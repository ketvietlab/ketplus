# The leader keeps its context, team messages arrive in batches, the leader can interrupt

Status: accepted direction; implementation tracked as team tasks T-9 to T-11.
Amended 2026-11-26: the leader's `urgent` steers again and interrupt is the last resort
([decision](2026-11-26-leader-steers-first-interrupt-last.md)); decision 2 below still holds
for every other sender.
Amends [ADR 0114](0114-agent-teams.md) §11 (context reset) and
[ADR 0027](0027-conversation-numbers-and-agent-messaging.md) (`urgent` delivery).

## Problem

1. The leader is reset like every member: after 12 turns, at 150,000 context tokens, or when
   the provider's cache has gone cold. The leader is the one member that works across tasks
   and answers the digests, so each reset makes it read a transcript and a 4 KiB briefing
   again, loses its working memory of the plan, and pays a cold-cache rewrite anyway.
2. An `urgent` message steers into the receiver's running turn. Members use it freely, so a
   turn is interrupted at every tool boundary by input that could have waited for its end.
   Every steer is another provider request with the whole context, and the receiver
   re-plans mid-edit.
3. The leader has no way to redirect a member that is going the wrong way. The only
   tools are a normal message, which waits for the member's turn to end, or `urgent`, which
   the member may read only after its current tool call.

## Decisions

### 1. The leader is never reset (amends ADR 0114 §11)

- `context_reset::decide` returns no reason for a member whose role is `leader`, under every
  policy, including `cache_cold` and `context_size`. Developers and reviewers keep the
  rules of §11 unchanged: task change, turn threshold, size, cold cache.
- The leader's context stays bounded by sending it less, not by forgetting it:
  - one digest per quiet period, as now, but the digest is **bounded** (at most 40 update
    lines and 8 KiB; a longer backlog is summarised as "N older updates on tasks …, read
    them with `team_tasks`") so a burst never costs more than a fixed amount of context;
  - the batching of decision 2 turns many small leader wakes into one;
  - the provider's own compaction handles what remains. The daemon does not compact.
- Settings and wire: `context_reset*` fields stay as they are (they still govern the other
  roles). `team_get` and the desktop do not need a new field. The leader's `contexts` mark
  is no longer written. A team document that already holds a leader mark ignores it.
- The briefing for a fresh context is still produced for developers and reviewers; the
  leader branch of the briefing code is removed.

### 2. A team message never steers; it waits for the end of the receiver's turn

- A message to a busy conversation is queued and delivered **together, in order**, in the
  one turn that starts when the receiver goes idle. This is already the rule for a normal
  message; this decision removes the exception.
- `agents_send(urgent)` no longer steers. The parameter is still accepted so that existing
  callers and prompts keep working. It only moves the message to the front of the receiver's
  queue. The receipt says `queued`; `steering` and `urgent_waiting` are no longer produced
  and the agent instructions and tool description stop naming them. The broker's
  steer path, `urgentWaiting_`, and the per-event retry are removed. Journal rows already
  written with `delivery: "steer"` and `turn.steered` stay readable.
- The user's own steer from a client (`session.steer`) is unchanged. It is the user's
  action, not an automatic one.
- The team budget counts the turns that actually start, not the messages queued. A batch of
  several messages for one member is one automatic turn.
- For members other than the leader, the coordinator's messages (review requests, findings,
  answered decisions, task handovers) are also held for one quiet period when the member is
  idle and another message for it is already in the outbox, so that a burst of changes
  starts one turn, not several. A message that starts a turn the member was blocked on is not
  delayed beyond that period.

### 3. The leader can interrupt a member: `team_member_interrupt`

A leader-only team tool. The daemon refuses it for any other role.

Input: `member` (number or name), `text` (the priority message, required), `reason`
(a short code for the audit trail, required).

Behaviour, in this order, and atomically with respect to the outbox:

1. The target must be a member of this team, not the leader itself, and the team must not be
   paused. Otherwise the call fails with the same errors as the other team tools.
2. If the member has a running turn, the daemon stops it as the user's Stop does: the
   provider turn is cancelled, a tool call in flight is abandoned, and uncommitted files in
   its worktree are left alone. The stop is recorded as `stopped_by: leader`, not as the
   user's Stop, so the stall watcher and `cancelWait` treat it as a normal turn end: the
   member's `team_wait` timers are kept.
3. The message is stored with the highest priority, ahead of anything queued for that
   member, and starts the member's next turn immediately. The queued messages follow in the
   same turn, in order, after it.
4. The interrupt counts as one automatic turn against the team budget.
5. The task history of the member's task (and the team update feed) records
   `leader interrupted <member>: <reason>`. The member's prompt names the sender as the
   leader and says its previous turn was stopped by the leader, and that its worktree and
   task state are unchanged.
6. The receipt reports `interrupted` (true when a turn was stopped), the message id and
   `delivery` (`starting`).

Limits: at most 3 interrupts per member per hour; the fourth fails with `interrupt_limit`
and says to send a normal message. A background job the member started (a managed
background task) is not stopped.

## Compatibility and security

- No new network surface. The tool is an existing MCP tool of the daemon; its gating is the
  role check in the daemon, never the prompt.
- `urgent` keeps its field name and type. A caller that waited for `steering` now gets
  `queued`; no caller is known to depend on the former.
- No paid-feature gating or private repository is involved.

## Tests

- `TeamContextResetTest`: the leader gets no reason for any policy or idle time; the other
  roles are unchanged.
- Broker: an `urgent` message to a busy receiver stays queued, comes first in the next
  batch, and never calls `steerAgentTurn`. Several queued messages are delivered as one turn
  and count as one automatic turn.
- `TeamAgentToolsTest`: `team_member_interrupt` is refused for developers and reviewers, for
  the leader itself, for a non-member and while paused; stops a running turn of a fake host;
  keeps the member's wait; orders its message first; records the audit entry; enforces the
  hourly limit.
- Digest: a backlog over the bound is summarised.

## Rollout

Three tasks, in this order of risk: the leader exemption (T-9), batched delivery without
steering (T-10), the interrupt tool (T-11, depends on T-10 for the queue priority). Update
`docs/decisions/0114-agent-teams.md` §11, `docs/decisions/0027-…` and
`docs/project-status.md` in the task that changes the behaviour. Raise the daemon version in
each task that changes daemon behaviour.
