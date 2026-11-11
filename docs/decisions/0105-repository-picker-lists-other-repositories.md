# ADR 0105: The repository picker lists another repository before switching

Status: accepted (Jay, 2026-11-10). Extends ADR 0015.

## Context

The mock's Repository & worktree picker lets you select a repository, shows that
repository's worktrees, and switches only when you confirm with **Use this
repository**. The app switched as soon as a repository was clicked, to the last
worktree used there or to the repository root, because `session.worktrees` lists
only the conversation's own repository. ADR 0015 already allows a local switch to any
exact Git worktree root.

## Decision

- `session.worktrees` accepts an optional `repository_path` from trusted local IPC.
  The daemon lists that repository's worktrees instead of the conversation's. The path
  must resolve to an existing Git worktree; otherwise the command fails with
  `invalid_repository`. Paging, search and the touched-first order are unchanged.
- Every listing returns `repository_path`, the listed repository's root. Every entry
  carries `main`, which is true for the repository's main worktree.
- Listing another repository changes no context and does not enrol it: its worktrees
  join the catalog, which phones see, only after the conversation switches there.
- The cloud already strips `repository_path` from phone commands and now also removes
  it from results, so phones keep their granted opaque catalog (ADR 0015).
- A desktop that sends `repository_path` and receives a result without it is talking
  to an older daemon. It shows "Restart KétPlus to list another repository's
  worktrees." instead of listing the wrong repository.
- The desktop picker follows the mock:
  - Selecting a repository lists its worktrees, and the current or last-used one is
    chosen.
  - Selecting a worktree only chooses it. **Use this repository** (or a double-click)
    sends `session.worktree.switch` with the context revision fence.
  - After the acknowledgement, the picker closes and shows "Next turn: <repository> ·
    <branch>".
  - **Create worktree…** opens the Create worktree card in place (ADR 0111 removed
    the Worktrees page).
  - A failure keeps the picker open with the error; Use retries.

The project version moves to 0.1.51 for the daemon change.
