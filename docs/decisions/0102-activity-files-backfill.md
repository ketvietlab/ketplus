# ADR 0102: Changed files of indexed groups are listed after open

Status: accepted; a journal indexed before `activity_files` lists its files in
bounded passes after open.

The new UI shows the files an activity group changed. Live indexing records them
in `activity_files` as each edit completes. A journal whose groups were indexed
before the table existed has none, and listing them inside the open would bring
back the full history scan that ADR 0097 removed.

The migration creates `activity_files` and an `activity_files_backfill` marker
table `(session_id, generation, after_group, after_cursor)` in one transaction.
When `activity_heads` already existed without `activity_files`, the same
transaction inserts a marker for every session. A failed migration leaves
neither table, so the next open repeats it.

`LocalJournal::backfillActivityGroups()` processes file markers only when no
`activity_backfill` marker remains, so files are listed from complete
membership. A pass takes the write lock as in ADR 0097. It reads at most 2,048
grouped items past the marker's `(group_cursor, first_cursor)` keyset in index
order. For each edit it reads the completed item from `replay_events` and
records its files, stopping after the item that reaches 1 MiB of payload. It
then advances the keyset, or deletes the marker when the items are exhausted.
A marker for a deleted session or an older generation is removed. A pass that
lists files or deletes the marker increments the session's
`replay_projection_state.revision`.

While either marker remains, `activity_summary` returns the session's events
ungrouped. A group's files are therefore never shown partially. An edit that
completes during the listing is recorded live; listing it again replaces the
row with the same files.

A fork copies `activity_files` with the other history tables. If the reader
snapshot still had a file marker for the source, the destination gets its own
marker. History purge removes `activity_files` in bounded passes (ADR 0093);
`session.delete` removes the file marker with the session's other state.

File markers are processed in insertion order. Unlike activity markers, a new
event does not move its session forward. Listing a session is bounded by its
grouped items, not its raw history, so the backlog drains quickly after the
activity index. No protocol or UI change is included.
