# ADR 0093: Bounded session history purge

Status: accepted; deletion commits atomically, physical history removal is bounded.

`session.delete` removed every row of a generation in one synchronous
transaction. A real `SessionHost` fixture measured 1,192 ms for 300k raw plus
300k replay rows and 256 MiB of chunks, blocking the daemon event loop and every
other journal writer for that time (#68).

The deletion transaction still commits the tombstone, provider/cloud cleanup
work, agent-state release, the `sessions` row and all small per-generation state
(background tasks, activity heads, replay projection, client cursors, staged
attachment manifests and contexts) together. Bulky history (large payload
manifests and chunks, staged attachment chunks, raw, replay and cloud events,
activity items and summaries) receives one bounded batch in the same
transaction. If any remains, a `session_history_purge` marker is inserted and
must affect exactly one row; otherwise the deletion fails and rolls back.

After commit the generation is unreachable: every read path is keyed by a live
session or joins `sessions`, and the tombstone prevents restoring that
generation onto leftover rows. Tombstone retention skips identities that still
have a purge marker. ADR 0055 is preserved: a failed or ignored write cannot
report success, and a statement that removes nothing while rows remain fails the
deletion instead of committing a tombstone over hidden history.

`SessionHost` drains markers one pass per event-loop turn after a deletion, at
startup and on the 15-second deletion-work timer. Each pass is one transaction
with `busy_timeout=0`: a busy writer defers it for 250 ms, and a pass commits all
of its progress or none of it. A pass that makes no progress fails, keeps the
marker and is retried by the timer.

Deleted history therefore stays on disk until the purge drains, normally within
seconds; it is never served. Low-space behaviour, process kill during a pass and
power-loss durability rely on SQLite transactions and are not separately
certified here. No schema change other than the new table, no command envelope
change and no UI change is included.
