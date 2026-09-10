# Git integration

KetPlus deliberately keeps Git support read-only. It shows repository status,
separates staged changes from working-tree changes, opens a diff scoped to the
selected group, and switches the active workspace between linked worktrees.
Staging, commits, branch creation, rebases, and remote operations stay in the
terminal.

## Commands

- `Source Control` opens with `Ctrl/Cmd+Shift+G`. Its `Staged Changes` group shows
  the index diff, while `Changes` shows only unstaged and untracked content.
- Selecting a file in Source Control opens its group-specific diff as a reusable
  editor tab. The inline view has old/new gutters, styled hunk headers, semantic
  added/removed line colors, and change counts instead of exposing raw patch
  metadata. Activating the Source Control row opens files that still exist.
- `View Current File Diff` is available from the Git menu and Explorer context
  menu; it shows the complete on-disk change against `HEAD`.
- `Refresh Status` asks Git for a fresh snapshot.
- `Switch Worktree` lists linked worktrees and opens the selected directory as
  the current workspace.

Switching worktrees does not run checkout or modify files. Open tabs remain open,
so unsaved work is not discarded. KetPlus remembers the most recently active
relative file for each worktree during the current app session; on first visit it
tries the same relative file that was active in the previous worktree.

## Implementation notes

The integration uses the installed `git` executable rather than embedding a Git
library. Commands run asynchronously with shell expansion disabled, pagination
disabled, and optional repository locks disabled. Status uses porcelain v2 with
NUL-separated records, and worktree discovery uses the porcelain worktree format.
Command output and execution time are bounded so a slow repository cannot grow
memory indefinitely or leave a process running forever.
