# ADR 0112: Waiting for you

Status: accepted (2026-11-11, #125).

## Context

A finished turn whose answer asks the user to approve or decide something looked
exactly like finished work: the sidebar showed the Completed tick, so approvals
sat unnoticed. `needsInput` covers only a running turn blocked on a question or a
permission request.

## Decision

- The daemon owns the state. `turn.completed` carries `"awaiting_reply": true`
  when a turn that finished normally (status empty, `completed` or `ready`)
  waits for the user. The flag is stored in the journal (`awaiting_reply`
  column), listed by `session.list` and survives a daemon restart. Feature:
  `session.awaiting_reply/1`.
- Two signals set it:
  - explicit: the agent calls the `ketplus` MCP tool
    `request_user_decision { summary }` during the turn (daemon operation
    `agents.request_user_decision`). Session instructions tell every agent to
    call it when the final answer needs approval;
  - fallback heuristic: the last paragraph of the final answer, after removing
    fenced code, is a direct question (ends with `?` or `？`, ignoring trailing
    emphasis, quotes and brackets). Tables and quotes do not count. A plain
    summary that asks nothing never sets it.
- It clears when the user sends a message, when a turn starts, when the
  conversation is forked, and on `session.mark_done` (row menu: Mark as done).
- Desktop shows **Waiting for you**: the sidebar row uses
  `ket::ConversationStatus::AwaitingReply` (`help` in `accent-text`), the header
  badge uses the Info tone. A running turn always wins over the flag.
- The Dock badge counts conversations waiting for the user and nothing else;
  unread completions keep their own row mark. A worktree shown in two windows
  counts once (`WaitingBadge`).
- The paired phone reads the same `awaiting_reply` field from `session.list`
  and from `turn.completed`, which the cloud projection passes unchanged. The
  phone app renders its own count.

## Consequences

- Turns of every provider are covered by the heuristic; agents that call the
  tool are marked even when their answer does not end with a question.
- Agent-to-agent replies sent with `expect_reply` are not inspected yet; a
  waiting conversation marks itself only through its own final answer or tool
  call.
