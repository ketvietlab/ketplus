# The leader steers first; interrupt is the last resort

Status: accepted. Amends [ADR 0114](0114-agent-teams.md) §11 and
[the 2026-11-25 decision](2026-11-25-team-batched-delivery-and-leader-interrupt.md) §2,
and [ADR 0027](0027-conversation-numbers-and-agent-messaging.md) (`urgent` delivery).

## Problem

The 2026-11-25 decision removed steering for every sender because members used `urgent`
freely and every steer re-planned a turn at a tool boundary. That left the leader with
two tools only: a normal message that waits for the end of the turn, and
`team_member_interrupt`, which ends the turn and can cut a build or test off halfway.
Something important that cannot wait a whole turn should not need the heavy tool.

## Decisions

1. **Only the leader steers.** `agents_send` with `urgent` from a team leader to a member
   of its own team is added to the member's running turn at the earliest point its
   provider allows. The receipt says `steering`; when the turn is in a tool call it adds
   `waiting_on` and a note (read when that call ends). When the member cannot be steered
   now it says `urgent_waiting` with `waiting_on`, and the message steers at the first
   point the provider allows, or starts a turn when the member goes idle. An idle member
   gets `starting`.
2. **Inside a team, only the leader may send `urgent`.** A developer or reviewer that sets
   it is refused with `urgent_leader_only`; the daemon decides by the sender's role, never
   by a prompt. A normal message between members is unchanged: queued, delivered in one
   batch when the receiver goes idle.
3. **Outside a team nothing changes.** A conversation on no team keeps the 2026-11-25
   behaviour: `urgent` only moves the message to the front of the receiver's queue and the
   receipt says `queued`. A leader's `urgent` to a conversation that is not a member of
   its team also only goes first in the queue.
4. **The leader's order is steer, then interrupt.** The leader interrupts with
   `team_member_interrupt` only when (a) the member is going the wrong way in a harmful
   or costly way (editing the wrong branch, about to merge the wrong thing, running
   something that spends provider quota), or (b) the steer's receipt was `urgent_waiting`
   and the matter cannot wait. Never to ask for status or for what can wait. An interrupt
   counts as one automatic turn, is limited to 3 per member per hour and can cut a build
   or test off halfway. The agent instructions and the tool descriptions say so.

## Implementation

The steer path removed on 2026-11-25 (`steerAgentTurn`, `urgentWaiting_`, the per-event
retry and the tool-call tracking behind `waiting_on`) is restored, with one change: a
message steers only when `AgentBroker::leaderSteer` holds (sent by an agent whose team
role is leader, to a member of that team). The journal, `session.steer` from clients and
the batching of normal messages are unchanged.

## Tests

`ketplus_team_agent_tools_tests`: the leader steers a running member (`steering`,
`turn.steered`), a developer and a reviewer are refused with `urgent_leader_only`, a normal
member-to-member message is still queued, and a leader's steer into a tool call reports
`waiting_on`. `ketplus_agent_urgent_delivery_tests`: outside a team `urgent` stays queued;
the instructions and `agents_send` description name every delivery state.
