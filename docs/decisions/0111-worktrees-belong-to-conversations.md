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
- A worktree can be removed with its conversation, from the worktree menu, or
  outside KetPlus. The desktop delete actions and daemon team cleanup refuse
  main checkouts, dirty checkouts (including ignored files and index flags
  that suppress change detection), and HEAD commits absent from the local
  remote-tracking refs. They check for processes using the checkout or its
  descendants immediately before Git removes it without force. Linux sees the
  cwd data exposed by procfs. macOS uses the current user's visible cwd entries.
  Windows relies on the operating system refusing to remove a process's cwd.
  These checks cannot prevent an external process from entering the folder after
  the check or see processes hidden by host permissions or namespaces.
- Conversation worktree ownership records the identity of the linked checkout.
  A missing checkout can remain visible as history. A checkout that replaces it
  at the same path does not inherit that ownership. Older records without an
  identity can still be shown, but cannot authorize deletion from a conversation.
  After a move, the execution picker can list the last recorded repository so
  the user can choose a new checkout without silently changing the conversation's
  execution path.

## Consequences

- KetPlus no longer runs `git status` for every linked worktree, so it does less
  work in the background.
- `AgentWorktreeCreator::prune` stays, but no UI calls it. A missing worktree
  folder still shows in its conversation (the missing-worktree state), but its
  stale Git entry is not cleaned up: `git worktree add` to the same path fails
  until `git worktree prune` runs. Successful in-app removal attempts to prune
  stale registrations, and a later prune error is logged separately.
- Worktrees made outside KetPlus with no conversation do not appear anywhere
  except Git > Switch Worktree and the New conversation base list.
