# ADR 0010: Conversation-owned Git worktree metadata

Status: accepted
Date: 2026-09-17

## Context

The Agent Workspace composer bar listed Git worktrees by scanning hydrated
transcript messages. Scrolling older pages then grew the bar with paths the
current conversation did not create. Git's worktree list has no conversation id,
and reconstructing ownership from the daemon journal would couple desktop Git
bookkeeping to transcript replay.

A conversation may own a primary linked worktree (created with New conversation)
and additional worktrees the agent created during a turn. Deleting that
conversation should be able to remove every owned checkout, not only the
conversation's current working directory.

## Decision

`conversation-worktrees.sqlite` lives in `QStandardPaths::AppLocalDataLocation`,
or `KETPLUS_CONVERSATION_WORKTREES` when tests override the path. The
`conversation_worktrees` table maps conversation id to canonical worktree path,
role (`primary` or `agent`), a hidden flag, and a created timestamp. The schema
contains no credentials or transcript content.

Writes happen at known desktop events:

- user-created conversation worktrees are stored as `primary` when the checkout
  is created;
- `turn/started` snapshots `git worktree list --porcelain -z`;
- `turn/completed` persists paths that appeared in that turn, skipping the main
  checkout, paths already owned by another conversation, and paths that turn's own
  transcript never names. Git records no creator, so a checkout made meanwhile by
  the user or another conversation is not claimed.

Both listings run `git` asynchronously; a result that arrives after the next
snapshot or capture is dropped. Deletion lists the exact checkouts it would
remove before the user opts in.

The composer bar renders only visible rows from this store. Each row looks up
that worktree's latest pull request with `gh pr list --head BRANCH --state all
--limit 1`. Dismissing a row sets `hidden`; it does not run Git. Conversation
deletion snapshots every stored path before removing the rows, then removes
eligible linked worktrees sequentially. The store is never rebuilt from the
daemon journal.

Every store operation owns a uniquely named Qt SQL connection on its calling
thread, matching ADR 0004. Reads are cached per conversation in memory and each
write drops that entry, since the conversation list re-reads ownership often. Missing or unreadable databases are empty results
and do not block chat.

## Consequences

Ownership survives restart without scanning history. Two conversations in the
same repository cannot claim one checkout. A failed `git worktree list` at turn
start skips capture for that turn rather than attributing the whole repository.
Hiding a row is presentation-only; delete still offers the hidden checkout for
cleanup.

## Alternatives considered

Scanning command receipts or the full journal would grow with history and still
miss Git worktrees created outside recorded commands. Relying on
`git worktree list` alone cannot assign a checkout to a conversation.
`QSettings` is suitable for conversation titles but not for a multi-row path
map with roles and hide state.
