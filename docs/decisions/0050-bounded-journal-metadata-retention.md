# ADR 0050: Bounded journal metadata retention

Status: accepted; physical age/cap cleanup is eventual.

Command results, remote audit, tombstones, orphan client cursors and orphan
background tasks previously used backlog-sized DELETEs at open or maintenance.
The Qt 256th-write regression removed 90,256 command/audit rows in one call.
Timestamp-ordered caps could also discard a just-accepted command or audit record
after wall-clock rollback, losing the command's idempotency result immediately.

Each table now has a separate transactional lane and durable `journal_metadata_gc`
age/cap rowid cursors. One lane visits at most 256 age/orphan candidates and,
for capped tables, another 256 cap candidates. These two scans share a deletion
limit of 256 rows and 1 MiB of stored column bytes. Candidate reads seek rowid
ranges; byte lengths come from `octet_length`, without hydrating large TEXT values.
Date parsing is limited to 64-byte timestamps; orphan session lookups accept only
keys up to 256 bytes and seek the session primary key with the generation guard.

The existing 10,000-row cap for command results and audit uses accepted-write
rowid order, independent of wall-clock order. A descending rowid scan finds the
10,001st retained row, so cutoff discovery navigates at most 10,001 entries,
regardless of backlog size, and never sorts the backlog. The cutoff is computed
after age deletions to avoid over-pruning. Duplicate ignored commands/audit IDs do
not become newer rows; an explicit command-result replacement is a new write.
The existing 7/90-day wall-clock age policies and audit listing order are unchanged.

Rows, cursor advancement and cursor creation/update commit together per table.
An ignored/aborted DELETE, failed state write or commit failure rolls back that
lane. Earlier independent lanes may already have committed. A busy writer defers
immediately and restores the prior busy timeout. An empty tail scan wraps once in
the same pass, still visiting at most the original candidate limit. This lets
reopen recheck earlier rows that have expired since the preceding small-table pass.
An eligible row that does not fit the remaining byte budget is retried next pass.
Unsupported storage types or individually oversized rows are retained and skipped
so other metadata can progress. This differs from raw delivery cleanup's required
contiguous-prefix rule (ADR 0049); these tables have no raw replay cursor contract.

Reopen and every 256 command results run one pass of each lane; every 256 new
audit records also run an audit pass. The existing idle maintenance timer now
continues all metadata lanes along with global payload GC. Budgets are **per
table**, not shared across the metadata tables or with raw/blob cleanup.
The daemon does not promise a hard row/disk quota or drain during continuous
active turns. Missing `octet_length` defers metadata cleanup and warns once rather
than falling back to loading unbounded values. Unsupported-data recovery,
staged-attachment/cloud cleanup and explicit deletion remain separate plan items.

`ketplus_daemon_metadata_retention_tests` reproduces the actual 256th-write path,
clock rollback, byte-limited reopen, age/cap shared budget, unsupported-row
fairness, orphan generation isolation, failure/commit rollback, busy deferral,
capability absence and the actual prepared candidate/parent query plans. Synthetic
fixtures do not constitute Windows, live-journal or mixed multi-GB acceptance.

ADR 0052 adds completed deletion jobs as a sixth independent lane; its eligibility
is durable completion, rather than age or a cap.

ADR 0053 adds acknowledged cloud-envelope cache as another independent lane,
using per-entry receipts for the exact sent ciphertext.
