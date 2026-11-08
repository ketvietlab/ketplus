# 0091: Paged session tombstone catalogs

Status: accepted
Date: 2026-11-08

## Context

`session.list` previously loaded every retained tombstone into one response. A
synthetic Qt SessionHost fixture with 100,000 tombstones produced 5,389,329 bytes;
300,000 produced 16,389,329 bytes. The local IPC frame limit is 1 MiB. This is a
transport failure as well as an unbounded read; silently returning a prefix
would leave persisted client conversations undeleted.

## Decision

The additive `session.list` request payload opts in with
`deleted_sessions_page_version: 1`. A reply supplies `deleted_sessions`, the same
version, and `deleted_sessions_next`. An empty next object means end of this
walk; otherwise it is the last returned `{session_id, session_generation}` pair.
Send that exact object as `deleted_sessions_after` for the next page. The
optional boolean `deleted_sessions_only` suppresses the live catalog even on the
first page. Continuations always suppress it.

Storage seeks the existing `(session_id,generation)` primary-key index, ordered
by both keys, with a 129-candidate limit. Each page returns at most 128 rows and
64 KiB of compact JSON array data. A row excluded by the byte budget is read
again on the next page. Cursor keys must be nonempty text up to 1,024 UTF-8 bytes
and a positive integer generation. SQL CASE projections check stored identity
bytes/types and generation types before Qt materializes them. Malformed rows or
read errors fail the whole page and retain the original database contents.
These checks are not WHERE filters that can silently skip an invalid tombstone.
The SQLite `octet_length` requirement/fail-closed behavior follows ADR 0085.

Without opt-in, the server returns the complete legacy result only when it fits
a fixed 32,768-row/960-KiB tombstone budget. Otherwise it returns
`session_catalog_upgrade_required`; it never labels a truncated list complete.
The final live-plus-deleted response also reserves 16 KiB below the IPC frame
limit and reports `session_catalog_too_large` instead of emitting an oversized
frame. This is an explicit upgrade boundary for large legacy catalogs, not
pagination support retrofitted into an old client.

The Desktop transport bridge reads one page at a time, checks row types/order,
byte/row limits and cursor progress, and applies each page immediately without
accumulating the history. A missing version is a legacy fallback only on the
first reply. Reconnection invalidates callbacks and timers from the old walk.
After the walk the bridge reads a fresh live catalog, so an old initial catalog
cannot resurrect a conversation deleted while pages were in flight. The final
catalog's first tombstone page may be applied again; deletion remains scoped to
its exact generation and is idempotent.

This is an ordered walk, not a long-lived database snapshot. Concurrent
`session.deleted` events and a fresh live catalog provide the current view;
reconnecting starts the walk over. Tombstones expiring under existing retention
are not resurrected by pagination. No UI layout or rendering behavior changes.

## Consequences

The synthetic backlog, real local IPC, legacy fallback, malformed continuation,
reconnect epoch, escaped-identity byte budget, mixed generations, removed cursor
row, actual prepared query plan and SQL read/type failures have focused tests.
Large 100k/300k fixtures require `KETPLUS_RUN_DELETE_BACKLOG_TESTS=1`; the normal
suite retains the smaller functional cases. No provider quota or live data is
used.

Explicit session deletion still removes all matching history in one atomic
transaction (ADR 0055). A 300k raw + 300k replay + 256-MiB chunk fixture blocks
its caller for about 1.2 seconds; this remains an open H1.1 gate. Live-session
catalog bounds, whole-history fork, crash/filesystem races and issue #68 closure
are not established by this change.

## Alternatives considered

Increasing the IPC limit merely postpones the same unbounded operation.
Truncating a legacy catalog without an error loses deletion propagation.
Accumulating all pages in the bridge avoids the frame limit but retains
backlog-sized memory. Keeping the initial live catalog until the end permits
stale resurrection, so the bridge obtains it after completing the walk.
