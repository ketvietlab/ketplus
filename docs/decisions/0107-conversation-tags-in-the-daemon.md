# ADR 0107: The daemon stores a conversation's tags

Status: accepted (Jay, 2026-11-10). Replaces the Desktop-only storage of ADR 0028.
Amended by [ADR 0117](0117-tags-leave-the-ui.md): the daemon keeps tags, the UI draws none.

## Context

The mock's conversation header has an **Add tag** button, and the sidebar rows and the
header draw a conversation's tags. The app could draw tags it was given, but nothing
stored them. ADR 0028 planned a Desktop SQLite table. A conversation belongs to the
daemon, though: its title, archive state and number live in the journal, and a second
window, a reset of the desktop settings or a later client must show the same tags.

## Decision

- The journal keeps the labels with the session row (`sessions.tags`, a JSON array of
  strings, `[]` by default). Like `archived_at`, only its own command writes the
  column; other writes of the row leave it alone, and it survives a restart.
- `session.tags.set` with `{tags: [label, …]}` replaces the list. It is a mutating,
  deduplicated command on trusted local IPC. The daemon trims each label and rejects
  the request with `invalid_payload` when `tags` is not an array, has more than 8
  labels, or a label is not a string, is empty, exceeds 64 UTF-8 bytes or contains a
  control character. The same label in another case is one tag; the first spelling
  stays. The result returns the stored `tags`.
- `session.list` returns `tags` for every session. Daemons that support this advertise
  the `session.tags/1` feature.
- A fork starts without tags.
- The daemon stores which labels a conversation has. Names on offer and their colours
  belong to the client: Desktop offers the mock's seven (UI, Performance, Review,
  Atlas, Account, Bug, Docs) and draws a label it does not know in the neutral colour.
- Desktop changes its list only after the daemon has stored the new one. Without
  `session.tags/1`, or before the conversation has started and the daemon knows it,
  **Add tag** is hidden.
- No transcript event is written: tags are catalog metadata, not conversation history.

## Consequences

Superseded in part by [ADR 0113](0113-tag-catalog.md): the catalog, Manage tags…,
tagging before the first turn, live updates and phone tags.

- Phones and the cloud projection do not carry tags yet; the cloud builds its own
  session summary and is unchanged.
- Another desktop window sees a change at its next catalog refresh, like archive state.
- User-defined labels, colours and **Manage tags…** need a catalog command and are a
  follow-up; the stored format (labels only) does not change for them.
