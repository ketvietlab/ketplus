# 0018: Conversations continue after their worktree is deleted

Status: accepted
Date: 2026-10-06

## Context

Agents often remove and recreate Git worktrees while they work, and users delete
finished checkouts. ADR 0015 rejected every turn of a conversation whose
worktree no longer existed (`worktree_missing`) until someone picked another
worktree. Agents could not finish their work, and the composer appeared locked on
the desktop and the phone.

## Decision

- `session.start_turn` no longer rejects a conversation whose recorded worktree is
  missing. The session keeps the recorded path. The provider runs in the
  repository's primary worktree, which is the root recorded by
  `worktreeCatalogRoot`. If that is unknown, it runs in the nearest surviving
  parent folder. The turn input tells the agent that the worktree is gone and
  where it runs.
- The daemon records the repository's worktrees when such a turn starts. When the
  turn completes, it moves the conversation to the replacement with the same
  `session.worktree.switch` journal transaction: the context revision increases
  and a `conversation.context_changed` event is emitted. The replacement is
  chosen in this order:
  1. a worktree with the deleted folder's name;
  2. otherwise the single worktree the turn added.
  If the folder is recreated at the same path, nothing changes. Several new
  worktrees are ambiguous and are left to the worktree picker.
- Clients follow `conversation.context_changed` as they already do for a manual
  switch. Their missing-worktree notice explains that the agent continues from
  the main worktree. It no longer asks the user to choose a worktree first.

## Consequences

- The Claude sidecar copies a resumed session's transcript into the project
  directory of the new cwd when Claude Code stored it under another one, so
  `resume` keeps working after the cwd changes.
- Each turn of a conversation without a worktree runs `git worktree list` on
  completion, bounded by the existing two-second Git timeout.

## Alternatives considered

- Keep rejecting turns until the user switches. This is the lock that users
  reported.
- Ask the agent to report the worktree it created through a tool. That is
  precise, but it depends on the provider, and the agent can forget to call it.

## Extension: existing working checkouts

Successful Codex tool evidence may also follow an existing same-repository worktree
at turn completion. The active turn ID and captured context revision fence this
choice; manual context selection wins. Commands in the submitted checkout used for
bookkeeping do not cancel stronger evidence of another working checkout. The original
missing-checkout replacement heuristic remains the fallback when no explicit tool
path proves a target.
