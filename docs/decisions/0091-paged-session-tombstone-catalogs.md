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
Send that exact object as `deleted_sessions_after` together with the reply's
opaque `deleted_sessions_snapshot` token (at most 128 characters) for the next
page. `daemon.hello` advertises `session.tombstones/1`. The
optional boolean `deleted_sessions_only` suppresses the live catalog even on the
first page. Continuations always suppress it.

Storage seeks the existing `(session_id,generation)` primary-key index, ordered
by both keys, with a 129-candidate limit. Each page returns at most 128 rows and
64 KiB of compact JSON array data. A row excluded by the byte budget is read
again on the next page. Cursor keys must be nonempty text up to 1,024 UTF-8 bytes
and a positive integer generation. SQL CASE projections check stored identity
bytes/types and generation types before Qt materializes them. Identities are
projected as bounded bytes and decoded with strict UTF-8 validation; Qt must not
silently replace malformed bytes and thereby change the identity. Malformed rows or
read errors fail the whole page and retain the original database contents.
These checks are not WHERE filters that can silently skip an invalid tombstone.
The SQLite `octet_length` requirement/fail-closed behavior follows ADR 0085.

Without opt-in, the server reads at most 32,769 candidates and 1 MiB of
serialized tombstone data. It returns the complete legacy result only when the
actual serialized protocol response, including correlation id, capabilities and
live sessions, fits the 1-MiB frame limit. Otherwise it returns
`session_catalog_upgrade_required` when paging can help, or
`session_catalog_too_large` for an oversized live catalog. It never labels a
truncated list complete or reserves an arbitrary gap below the wire limit.

A singleton journal revision changes transactionally through INSERT/UPDATE/DELETE
triggers on `session_tombstones`, including retention and older writers. Rollback
restores the previous revision. Each token combines this revision with the open
journal instance's random identity: a reopen invalidates old cursors. A read
checks the token before and after querying and again before publishing the live
catalog. A change yields `session_catalog_changed`, including insertion before
the keyset cursor. Missing or oversized continuation tokens are rejected. This
is a revision fence with restart, not a long-lived SQL snapshot. The revision
column is type/length checked before Qt reads it; an invalid/missing revision
fails closed.

The shared remote `SessionCatalogReader` is used by Desktop refresh and
`ketplusctl sessions`. Incremental discovery uses the live-only request in
[ADR 0092](0092-live-only-session-discovery.md). It reads one page at a time, checks row
types/order, byte/row limits and cursor progress, and streams each valid page to
its consumer. A missing version is a legacy fallback only on the first reply.
Reconnection invalidates Desktop callbacks from the old walk. Catalog changes
restart from the beginning, at most three times, then return an explicit error.
After EOF it fetches fresh live sessions under the same revision token; it never
publishes a stale initial live catalog. Deletion application remains scoped to
the exact generation and idempotent across retries.

The CLI writes tombstones to an automatically removed, private temporary file,
resetting it on each retry. Only a complete successful walk is streamed to stdout
as the existing single JSON object, using 64-KiB reads. This preserves the output
contract without retaining all tombstones in RAM. Raw `ketplusctl call
session.list` remains a single RPC and accepts explicit paging payloads.

A walk still visits all retained tombstones. Delta/since synchronization and total
refresh work are separate bounds, not established by the per-page and retry caps.
No UI layout or rendering behavior changes.

## Consequences

The synthetic backlog, real local IPC, legacy fallback, malformed continuation,
concurrent insertion behind the cursor, rollback/reopen fences, bounded retries,
discovery, CLI output above 1 MiB, reconnect epoch, escaped-identity byte budget,
mixed generations, removed cursor
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
