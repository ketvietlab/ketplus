# Execution worktree mock review — 2026-09-30

User selected the existing Két Design System and requested stopping after the mock for review.

- Header picker changes the next execution worktree within the same conversation.
- Keeps conversation identity, transcript and draft; workspace navigation and Atlas source remain separate.
- Busy/offline disable switching. Stale/revoked states require Refresh before choosing again.
- This is synthetic interaction, not a deployed API or native UI acceptance result.

KetAtlas 0.5.2 strict audit passed. KetPlus Browser inspected light/dark picker screens
and exercised switching. Mobile A→B→A retained the typed draft. Busy/offline choices
were disabled and stale/revoked recovery states were present on both platforms.
Viewer-wide theme synchronization and native end-to-end behavior were not re-certified.

Screenshots were retained locally during review; this change does not include
portable screenshot artifacts.

Desktop review adjustment: removed the touched-worktree / PR list above the composer. Execution worktree selection remains in the conversation header.

Worktree options and the pinned current worktree show a “Model used” badge for recorded model activity in this conversation (synthetic fixtures). Selection alone does not add this badge. Unknown historical activity remains unmarked; no backfill is implied.

Review refinement: model activity is indicated by a small ✦ icon beside the worktree name, with an accessible label and tooltip, replacing the text badge.

Model-touched worktrees sort before untouched worktrees in the initial list, browse-all catalog, and search results, before pagination. The current worktree remains pinned separately.
