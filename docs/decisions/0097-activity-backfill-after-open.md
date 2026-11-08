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
the write lock with `BEGIN IMMEDIATE` under `busy_timeout = 0`, so a busy journal
defers the pass. It picks one marker and indexes, in one transaction, at most
2,048 replay rows past the head and stops after the row that reaches 1 MiB of
payload. When the history is exhausted it settles the head at `last_cursor` and
deletes the marker. A marker for a deleted session or an older generation is
removed. A marker delete that removes no row fails the pass and rolls it back.
`SessionHost` runs one pass per event-loop turn while markers remain. It starts
them from the same startup and 15 s maintenance work that drains deletions, and
after a fork. A deferred pass retries after 250 ms; a failed pass keeps its
marker for the next maintenance run.

A new event for a marked session is not indexed when it is appended. Indexing it
before its history would group it wrongly, and indexing the history first would
scan it inside the append. The event's marker is moved to the front instead, and
the backfill indexes the event in cursor order. A fork whose copied history is
past its head gets its own marker. `session.delete` removes a session's marker
with its other state.

A group is published only after its session is fully indexed. While a marker
remains, `activity_summary` returns the session's events ungrouped and
`session.activity_group_page` answers `unknown_group`. Every pass that indexes
rows increments the session's `replay_projection_state.revision`, so a page
snapshot taken before a membership change answers `snapshot_changed`. Rows
indexed later are the same as rows indexed live.

The fixture now opens in 31 ms; its slowest pass takes 10 ms. With all 300k rows
in one session, an append to it takes under 1 ms and the slowest pass 34 ms. No
protocol change, migration of an existing table or UI change is included.
