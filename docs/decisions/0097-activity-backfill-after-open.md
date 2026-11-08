# ADR 0097: Activity index backfill runs after open

Status: accepted; opening a journal no longer indexes activity history.

`initializeActivityGroups()` indexed, inside the open, every session whose
replay rows were newer than its activity head. On the first open after the
activity tables appeared this covered the whole history: a temporary Release
fixture with 300k raw and 300k replay rows over 20k sessions spent 5.3 s of a
5.6 s open there (#68). A failure in that work also failed the open.

Open now only records the work. For each session whose `last_cursor` is past its
`activity_heads.indexed_cursor`, it inserts `(session_id, generation)` into an
`activity_backfill` marker table. `LocalJournal::backfillActivityGroups()` takes
the oldest marker under `busy_timeout = 0`, indexes up to 2,048 replay rows past
the head in one transaction, and deletes the marker and settles the head when a
pass reads fewer rows than the limit. A marker for a deleted session is removed.
`SessionHost` runs one pass per event-loop turn while markers remain, from the
same startup and 15 s maintenance work that drains deletions; a busy journal
defers the pass and a failed pass leaves the marker for the next attempt.

A new replay event for a marked session first indexes that session's earlier
rows below the event's cursor, so live grouping never sees a gap. A fork whose
copied history is past its head gets its own marker. `session.delete` removes a
session's marker with its other state.

Until a session's marker is drained, `activity_summary` and
`session.activity_group_page` may return its older activity ungrouped. Rows
indexed later are the same as rows indexed live. The fixture now opens in 30 ms
and its slowest pass takes 9 ms. No protocol, schema migration of existing
tables or UI change is included.
