# ADR 0111: Worktrees belong to conversations

Status: accepted (2026-11-11, #139).

## Context

The desktop app and the Qt mock had a Worktrees screen. It listed every linked
worktree with its Git state, read by one `git status` per worktree
(`WorktreeStateProbe`). It also offered Prune, Archive, Delete and "New
conversation here". The same actions were already available from the
conversation: New conversation creates a worktree, the execution picker switches
the next turn to another checkout, and deleting a conversation can remove its
worktree. Two places did the same job, and the screen showed worktrees with no
conversation as if they were work of their own.

## Decision

- Remove the Worktrees screen from the mock (`WorktreesScreen.qml`, its sidebar
  entry and the quick-search command) and from the app
  (`WorktreesWorkspacePage`, `WorktreeStateProbe` and Git > Switch Worktree >
  Manage worktrees…).
- A worktree is created only for a conversation: from New conversation, or from
  the repository picker's **Create worktree…**, which now opens the Create
  worktree card in place ("Start a conversation in it").
- A conversation switches its worktree with the execution picker
  (`session.worktree.switch`, ADR 0105). The editor's Git > Switch Worktree menu
  still lists the linked worktrees.
- A worktree is removed with its conversation (the delete card's linked-worktree
  options) or outside KetPlus.

## Consequences

- KetPlus no longer runs `git status` for every linked worktree, so it does less
  work in the background.
- `AgentWorktreeCreator::prune` stays, but no UI calls it. A missing worktree
  folder still shows in its conversation (the missing-worktree state).
- Worktrees made outside KetPlus with no conversation do not appear anywhere
  except Git > Switch Worktree and the New conversation base list.
