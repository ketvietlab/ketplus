# ADR 0113: The tag catalog lives in the daemon

Status: accepted (2026-11-11, #429). Extends ADR 0107. Amended by
[ADR 0117](0117-tags-leave-the-ui.md): the daemon part stands; the Desktop part below
(catalog reads, drawing, Manage tags…, tagging before the first turn) was removed.

## Context

ADR 0107 stored which labels a conversation has, but the names on offer and their
colours were fixed in Desktop. There was no **Manage tags…**. A conversation could
not be tagged before its first turn. Other windows saw a change only at the next
catalog refresh, and phones did not see tags at all.

## Decision

- The daemon keeps one catalog for this Mac: an ordered list of `{label, color}`.
  It is stored as one row of the journal's `tag_catalog` table, which is created
  on first use. Until a catalog is saved, the daemon offers the seven defaults
  (UI, Performance, Review, Atlas, Account, Bug, Docs). An empty saved catalog stays
  empty. Feature: `tags.catalog/1`.
- `tags.catalog` reads the catalog.
- `tags.catalog.set {tags, renames}` replaces the catalog. It is a mutating,
  deduplicated command:
  - it accepts at most 32 tags, each with a printable label of 1 to 64 bytes and a
    `#rrggbb` colour, every label unique regardless of case;
  - `renames` maps an old label to a label in the new catalog;
  - a conversation keeps a renamed label under its new name;
  - a conversation loses a label the catalog no longer lists, unless the label was
    never in the old catalog;
  - labels outside the catalog stay on conversations and are drawn in the neutral
    colour.
- Live updates use a push, not a journal event: `tags.changed`, with `catalog` and/or
  `sessions: [{session_id, session_generation, tags}]`. The daemon sends it to every
  local desktop client after a catalog save or a `session.tags.set`. It is not
  replayed; a client reads the catalog again when it connects. This keeps tags out
  of the transcript, as ADR 0107 requires.
- Desktop:
  - reads the catalog when the daemon is ready;
  - draws tags in catalog colours;
  - applies `tags.changed` to every window, except while that window has its own
    request in flight;
  - **Manage tags…** sits at the end of the Add tag menu. It renames, recolours,
    removes and adds labels on a draft and saves the whole catalog, as the mock's
    `parts/ManageTagsDialog.qml` draws it.
- Tagging before the first turn: **Add tag** shows as soon as the daemon stores tags.
  Labels chosen before the daemon knows the conversation stay in the window. They are
  sent with `session.tags.set` once the first `turn/start` succeeds, because that is
  when the daemon creates the session.
- Phones read `tags` (`[{label, color}]` in catalog colours) and `awaitingReply`
  (#125) from the sealed session metadata in the heartbeat. Neither is in the
  plaintext snapshot.

## Consequences

- Two windows editing the catalog at the same time: the last save wins, and both
  windows show it right away.
- Tags chosen before the first turn are lost if the window closes before that turn
  starts.
