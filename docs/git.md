# Git integration

KetPlus keeps the shared Git service read-only. It shows repository status,
separates staged changes from working-tree changes, opens a diff scoped to the
selected group, and switches the active workspace between linked worktrees.
The Agent Workspace has narrowly scoped worktree mutations: its add button creates a
new branch and linked worktree, and its context menu safely removes a linked
worktree without deleting the branch. Staging, commits, rebases, and remote operations
stay in the terminal.

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
- `Create Worktree` in Agent Workspace asks for a branch name and a base branch.
  The base selector loads local and remote-tracking refs asynchronously without
  fetching, defaults to the current local branch, and offers HEAD for detached
  worktrees. Git receives the selected full ref as the explicit start point.
  For a branch
  such as `feature/agent-ui`, it derives a sibling directory named
  `<repository>-feature-agent-ui`, runs `git worktree add -b`, and switches to it
  after Git succeeds.
- Right-click a worktree in Agent Workspace to delete it after confirmation.
  Only inactive linked worktrees without a running agent are eligible. The
  branch is retained; Git refuses dirty or locked worktrees unless conversation
  deletion explicitly requests a forced remove.
- New conversation picks an existing Base worktree or creates a linked
  worktree from that checkout by entering a branch name. Conversation-owned
  worktrees are recorded in desktop SQLite when that create succeeds and when
  a turn completes with new Git worktrees. The composer bar and
  conversation-delete cleanup use that map, not a transcript scan. See
  [ADR 0010](decisions/0010-conversation-owned-worktrees.md).

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

Status refreshes on its own when Git runs outside KetPlus, for example in the
integrated terminal or in an agent. The service watches the repository's git
directory (index and `HEAD` replacements) and its `logs/HEAD` reflog, and
debounces changes into one refresh. Its own `git status` runs with optional
locks disabled, so it doesn't write the index and trigger itself. The Source
Control list keeps the selected file and the scroll position across refreshes.

Only the newest diff request is answered. Replies to an older request, or to a
request made before a workspace switch, are dropped, so a slow diff can't
replace the one the user asked for last. Routine diff errors, such as a file
outside the repository or Git's own error message, appear in the status bar
instead of a dialog.
