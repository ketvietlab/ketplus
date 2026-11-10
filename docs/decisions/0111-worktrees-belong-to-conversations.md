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
- A worktree is created only for a conversation:
  - from New conversation, which starts a conversation in it;
  - from the repository picker's **Create worktree…**. The card opens in place on the
    repository the picker lists (not the open folder). After Git creates the worktree,
    KetPlus sends `session.worktree.switch` with the picker's context revision, so the
    conversation that opened the picker runs its next turn there. The open folder does
    not change. The worktree is recorded as that conversation's, so deleting the
    conversation offers to remove it. If the switch is refused (for example a stale
    revision), the worktree stays recorded and the status bar says to choose it in the
    picker.
- A conversation switches its worktree with the execution picker
  (`session.worktree.switch`, ADR 0105). The editor's Git > Switch Worktree menu
  still lists the linked worktrees.
- A worktree is removed with its conversation (the delete card's linked-worktree
  options) or outside KetPlus.

## Consequences

- KetPlus no longer runs `git status` for every linked worktree, so it does less
  work in the background.
- `AgentWorktreeCreator::prune` stays, but no UI calls it. A missing worktree
  folder still shows in its conversation (the missing-worktree state), but its
  stale Git entry is not cleaned up: `git worktree add` to the same path fails
  until `git worktree prune` runs.
- Worktrees made outside KetPlus with no conversation do not appear anywhere
  except Git > Switch Worktree and the New conversation base list.
