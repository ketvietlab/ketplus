# Issue 68 acceptance dossier

Owner: #2739. Independent reviewer: #7705. Updated: 2026-11-08.
Original issue: https://github.com/ketvietlab/ketplus/issues/68

This maps the original seven criteria to evidence and remaining work. It is not
an issue-level PASS. Pending packages must pass review and integration before
counting as merged fixes. No UI, live journal, provider quota or deployment work.

| Original criterion | Available evidence | Remaining acceptance / owner |
| --- | --- | --- |
| Forward-only, idempotent migrations; newer-daemon journal safety | Startup transaction failures #250; pending #273 closes failed storage objects; pending #274 protects migration staging | Unknown/newer schema preservation, interruption/low-space and large-index startup / #2739 |
| Transactions, crash consistency and surfaced errors | Append/delete/fork/replay fixes #241–250; pending #254 prepares media ownership before journal commit | Cross-file interruption and broader read/commit failure coverage; do not infer power-loss atomicity from callback order / #2739 |
| Bounded/indexed reads on large journals | GC #220, ACK #223, metadata #228, staging #230, deletion queue #235, cloud cache #237, cancellation #240; pending #272 bounds stored policy JSON | Explicit session-delete and whole-history fork work; actual query plans and representative payload mix / #2739 |
| Retention and deletion remove rows/media | Transactional cleanup and retention tests above; media cleanup/dedup #251–252 | Physical media/blob retirement, shared-reference preservation and steady-state acceptance; #128 archive behavior is not implied / #2739 |
| Media paths, limits, permissions, orphan cleanup | #251–252 plus pending publication/fork/preview #253–255; pending private migration staging #274 | Ancestor/replacement races, decode/concurrency bounds, write-admission/orphan recovery and platform permissions / #2739 |
| Note natural splits for oversized LocalJournal.cpp | At combined journal code 6347493f it is 1,462 lines. Natural responsibilities listed below; current constructor fix is two net lines | Record this inventory in review; any actual extraction must preserve behavior and has its own validation / #2739 |
| Tests for the above | Each merged fix links focused fault/regression evidence; current combined media and journal matrices are separate gates | Independent final-head reviews and missing acceptance scenarios above; no whole-issue PASS until reconciled / #2739, #7705 |

## Existing package order

1. #255 contains #253 and #254. Integrated code `74804ad8` preserves their
   production/test content and includes integration `b3af8ed5`.
2. #274 contains #272 and #273. Integrated code `6347493f` preserves each original
   package's production/test content and includes the same integration baseline.
   Final #255 `dae6a297` is included at combined code `5a8bca1c`; full Release
   Pro build and 104/104 targets pass first run in 427.64 s. Independent review
   remains required. Media alone passed 104/104 at `74804ad8` in 456.04 s.
3. Merge reviewed packages with exact-head author evidence; resolve their shared
   documentation and daemon-test additions without dropping either side.
4. Continue the existing H1.1 explicit session-delete bound gate. Publish a
   concrete finding or acceptance evidence, then finish remaining criteria in
   this dossier. Do not start another domain merely because a PR was opened.

## LocalJournal.cpp ownership inventory

The remaining large file groups startup/schema and historical backfills;
replay projection; session metadata/deletion and payload pruning; append-event
transaction assembly; and command-result persistence. These are natural behavior
boundaries. Existing fork, replay reads, policy, ACK cleanup, attachments and
retention units already demonstrate the split pattern. Avoid an arbitrary line
split or a broad refactor while checking a two-line constructor failure fix.

## Next bounded-work characterization

Source inspection at combined code `5a8bca1c` identifies the concrete next gate:
`LocalJournal.cpp::deleteSession` deletes every row for a generation across its
tables in one synchronous transaction, then checks that no history remains.
`LocalJournalDeletion.cpp::sessionTombstones` materializes the complete tombstone
query. These observations are not a measured latency/memory regression yet.

Use a temporary Qt fixture with large raw/replay/chunk and tombstone backlogs;
measure the real SessionHost path and verify rollback, repair, duplicate deletion
and wrong-session preservation. Preserve #241's no-false-success guarantee when
choosing a bounded implementation. Do not silently turn an atomic deletion into
an acknowledged partial deletion merely to meet a row budget.

## Limits

A synthetic 9-GiB migration (#202) proved its own retained hashes, integrity,
idempotence and reclamation. It does not prove mixed-payload steady-state bounds,
all crash points, a real user's journal or Windows permissions. Keep #68 open
until all findings are fixed or explicitly accepted with evidence. #128's mixed
UI/backend criteria and #85's operator rollout gate remain separate.
