# Issue 68 acceptance dossier

Owner: #3329 (from #2739). Reviewer: #2739, single pass. Updated: 2026-11-08.
Original issue: https://github.com/ketvietlab/ketplus/issues/68

This maps the original seven criteria to evidence and remaining work. It is not
an issue-level PASS. Pending packages must pass review and integration before
counting as merged fixes. No UI, live journal, provider quota or deployment work.

| Original criterion | Available evidence | Remaining acceptance / owner |
| --- | --- | --- |
| Forward-only, idempotent migrations; newer-daemon journal safety | Startup transaction failures #250; #273 closes failed storage objects; #274 protects migration staging (merged `c3490da5`) | Newer format refused unchanged (ADR 0095); legacy backfills once (ADR 0096); activity backfill after open (ADR 0097); full disk and killed migration verified without a production change / #3329 |
| Transactions, crash consistency and surfaced errors | Append/delete/fork/replay fixes #241–250; #254 (merged through #255) prepares media ownership before journal commit | Cross-file interruption and broader read/commit failure coverage; do not infer power-loss atomicity from callback order / #2739 |
| Bounded/indexed reads on large journals | GC #220, ACK #223, metadata #228, staging #230, deletion queue #235, cloud cache #237, cancellation #240; #272 bounds stored policy JSON (merged through #274) | Statement audit of 25 operations (100k-event session, 200k events in 2,000 others, large results with orphans) passes after two bounded-read fixes; bytes, memory and media files need their own evidence / #3329 |
| Retention and deletion remove rows/media | Transactional cleanup and retention tests above; media cleanup/dedup #251–252 | Physical media/blob retirement, shared-reference preservation and steady-state acceptance; #128 archive behavior is not implied / #2739 |
| Media paths, limits, permissions, orphan cleanup | #251–252 plus merged publication/fork/preview #253–255; merged private migration staging #274 | Ancestor/replacement races, decode/concurrency bounds, write-admission/orphan recovery and platform permissions / #2739 |
| Note natural splits for oversized LocalJournal.cpp | At combined journal code 6347493f it is 1,462 lines. Natural responsibilities listed below; current constructor fix is two net lines | Record this inventory in review; any actual extraction must preserve behavior and has its own validation / #2739 |
| Tests for the above | Each merged fix links focused fault/regression evidence; current combined media and journal matrices are separate gates | Independent final-head reviews and missing acceptance scenarios above; no whole-issue PASS until reconciled / #2739, #7705 |

## Existing package order

1. #255, containing #253/#254, merged `691d18c4` after Pro104/104 and independent
   #7705 daemon56/56. All three PRs are MERGED; integrated code `74804ad8`
   preserves their production/test content.
2. #274 contains #272 and #273. Integrated code `6347493f` preserves each original
   package's production/test content and includes the same integration baseline.
   Final #255 `dae6a297` is included at combined code `5a8bca1c`; full Release
   Pro build and 104/104 targets pass first run in 427.64 s. Independent #7705 review PASS at `c30fc59e` (daemon56/56);
   merged `c3490da5`, including #272/#273. Integration merge `b999fc3b` has the same tree as the
   pre-merge evidence head `164d5da8`; no code/test changed. Media alone passed
   104/104 at `74804ad8` in 456.04 s.
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

## Bounded-work characterization

Source inspection at combined code `5a8bca1c` identifies the concrete next gate:
`LocalJournal.cpp::deleteSession` deletes every row for a generation across its
tables in one synchronous transaction, then checks that no history remains.
`LocalJournalDeletion.cpp::sessionTombstones` materializes the complete tombstone
query. These were initially source observations; the measured follow-up below now
quantifies latency and the IPC-size failure.

The temporary Qt fixture now covers large raw/replay/chunk and tombstone
backlogs, the real SessionHost path, duplicate deletion and wrong-session
preservation. The existing injected rollback/repair cases remain in the same suite. Preserve #241's no-false-success guarantee when
choosing a bounded implementation. Do not silently turn an atomic deletion into
an acknowledged partial deletion merely to meet a row budget.

## Limits

A synthetic 9-GiB migration (#202) proved its own retained hashes, integrity,
idempotence and reclamation. It does not prove mixed-payload steady-state bounds,
all crash points, a real user's journal or Windows permissions. Keep #68 open
until all findings are fixed or explicitly accepted with evidence. #128's mixed
UI/backend criteria and #85's operator rollout gate remain separate.

## Measured follow-up after #274

The temporary real SessionHost fixture measured 1,192 ms for atomic deletion of
300k raw + 300k replay rows and 256 MiB chunks; unrelated history, duplicate
operation/revision fencing and reopen remained correct. Tombstone catalogs at
100k/300k rows encoded to 5,389,329/16,389,329 bytes, exceeding the 1-MiB IPC limit.
The active fix adds versioned bounded pages and sequential bridge consumption
(ADR 0091). Explicit atomic-delete latency is still open; no partial deletion is
acknowledged as complete. Final Pro104/104 and focused scale/bridge gates pass at `221eb2f8`;
independent review and merge were completed by the #280 follow-up below. See
[evidence](evidence/h1-tombstone-pagination-qt.json).

The #274 review also noted that successful compaction with retained staging
artifacts does not surface `result.error` in `compactJournal`; preserve this
operator-diagnostics follow-up alongside broader migration/crash acceptance.

The #280 follow-up adds a revision/reopen fence and covers every built-in
`session.list` consumer: Desktop refresh, incremental discovery, and CLI. A
changed catalog restarts at most three times; the CLI spools history privately.
Concurrent insertion behind the cursor must be observed on restart, even with
no deletion broadcast. Code `96038758` passes full Pro104/104 and scale/bridge/CLI3/3
(deletion46, bridge30+1 live skip, headless6). Negative controls catch the missing
fence and old discovery/CLI callers. #7705 independently passed final head
`8ec98aee` (full Pro104/104); #280 merged at `fdec1645`;
[source and log hashes](evidence/h1-tombstone-snapshot-callers-qt.json) record the
new evidence. Delta/since refresh work and atomic deletion latency remain
separate from this correction.


Independent #280 review also measured 100k tombstones causing 857 ms and a
full batch of deletion signals on each incremental discovery. ADR 0092 separates
that path into a single live-only request. Initial/reconnect and CLI reconciliation
remain complete; per-row revision-trigger writes and total refresh work are not
claimed bounded. Release affected5/5 pass at `38ebde39`; 20k/100k discovery emits
zero additional deletion signals, while the previous code repeats all 20k/100k.
[Evidence and controls](evidence/h1-live-session-discovery-qt.json) record the exact
source and logs. Independent review/merge remains pending. The next separate
acceptance gate is still atomic session-delete work.

## Bounded session history purge (ADR 0093)

`deleteSession` now commits the tombstone, cleanup work, per-generation state and
one bounded batch of bulky history; any remainder gets a `session_history_purge`
marker in the same transaction. `SessionHost` drains it one pass per event-loop
turn; tombstone retention waits for the marker. Release daemon build, fixture
checkpointed before timing (`KETPLUS_RUN_DELETE_BACKLOG_TESTS=1`):

| Rows per event table / chunk bytes | Delete command | Purge passes | Slowest pass |
| --- | --- | --- | --- |
| 1k / 64 MiB | 17 ms | 30 | 0 ms |
| 100k / 256 MiB | 38 ms | 127 | 30 ms |
| 300k / 256 MiB | 30 ms | 146 | 35 ms |

The previous synchronous delete took 1,192 ms for the 300k case. Daemon suite
56/56 passes. Ignored purge markers, stalled passes, retention while pending,
event-loop draining and reopen are regressions. Whole-history fork bounds and the
remaining gates above stay open.

#2739's single review found two P2s, both fixed with regressions and negative
controls: cloud event upload re-read residual history on a 413 retry (now fenced
by live generation at entry and in the reply callback; cached restore pages are
also fenced), and an ignored marker delete reported progress (now fails). Batch
limits are documented as row counts, not a byte budget. Daemon suite 57/57.

## Staged session fork copy (ADR 0094)

Whole-history fork copied every row in one synchronous transaction: 1,899 ms
for 100k and 2,952 ms for 300k rows per event table with 256 MiB chunks. The
fork now copies one read snapshot in bounded passes; begin 3–7 ms, finish
0–1 ms, slowest pass 17 / 73 / 29 ms for 1k / 100k / 300k (Release daemon,
`KETPLUS_RUN_FORK_BACKLOG_TESTS=1`). Total copy time roughly doubles (5.7 s at
300k) without a long stall. Daemon suite 57/57. Newer schema, low space,
interruption beyond fork staging, media races and steady-state bounds remain.

## Journal format version (ADR 0095)

There was no format marker, so an older daemon would migrate and compact a
newer journal. The journal now records `PRAGMA user_version`; `LocalJournal`
and startup compaction refuse a newer format before any write, verified
byte-identical by `ketplus_daemon_journal_space_tests`, with negative controls
for each guard. Pre-versioning journals are upgraded after their migrations
succeed. Daemon suite 57/57. Low space and interruption beyond fork staging,
media races and steady-state bounds remain.

## Large-journal startup (ADR 0096)

A Release fixture (300k raw + 300k replay rows, 20k sessions) opened in 5.6 s
the first time (activity-index backfill 5.3 s, one-time) and 360 ms on every
later start: the turn-start and turn-number/title backfills rescanned sessions
whose values stay empty. Both now run only below format 1; later opens take
16 ms. A behavioural test fails when the gate is removed.

The activity backfill now runs after open (ADR 0097): open records markers and
the daemon indexes at most 2,048 rows or 1 MiB of payload per pass. Events for a
marked session wait for its backfill, and its groups stay unpublished until it is
indexed. The same fixture opens in 31 ms the first time; the slowest pass is
10 ms. With 300k rows in one session an append takes under 1 ms and the slowest
pass 34 ms. Drained rows match live indexing. Negative controls for event
deferral, the group-page gate, the byte budget, `BEGIN IMMEDIATE` and the marker
row check each fail their test.

## Migration interruption and low space

Opening a pre-format journal runs idempotent steps (`CREATE ... IF NOT EXISTS`,
column checks, backfills in their own transactions) and stamps the format last.
A recovery test now shows that this order holds. Its fixture has 60k rows in
2,000 sessions that number their own cursors, so every session takes the
turn-number and turn-start backfills; five later columns, the turn indexes and the
activity tables are removed. With 256 KiB left on a disk image, the open fails with
SQLite's full-disk error, the journal passes `integrity_check`, keeps format 0 and
unchanged events, and the next open with space matches an uninterrupted migration
(schema definitions, nine tables, the worktree catalog without its clock). Killing a
child process at six times of a 1.6–1.7 s migration (all six after some step had
committed) always reopens to that result. Stamping the format first, an index of
the same name on other columns and a wrong catalog repository each fail the test.
No production change was needed.

Limits: the child opens only `LocalJournal`, not the startup move to incremental
vacuum (covered by its own tests); kills are timed, not placed in each step; power
loss and journal-plus-media interruption are not covered.

## Query audit on a large journal

`JournalQueryAuditTest` traces each statement of 25 operations, on the journal
connection and on the fork's snapshot reader. The conversation in use has 100k
events of its own and recent turns written through the journal; 2,000 other
sessions hold 200k raw and replay rows, there are 20k tombstones, and a third
conversation holds six large tool results whose raw rows were pruned, three of
them orphaned. A statement fails above 5,000 full-scan steps or 200k VM steps, or
with an automatic index. The garbage pass must remove exactly the three orphans.

The audit found two history-sized reads, both fixed:
- `session.activity_group_page` walked every replay row of the session before
  `after_cursor` (1.18M VM steps). It now reads only between the group's first item
  and its summary's last change (461 VM steps).
- Each finished fork table ran `COUNT(*)` over the source and the fork (about 300k
  VM steps each for 100k rows), in one pass. `beginFork` now refuses an identity
  with leftover rows instead, so one stored row per snapshot row proves the copy.
  Wall time of the opt-in fork characterization did not change measurably (slowest
  pass 24–47 ms either way).

Controls: defeating the key in `eventsAfter`, in the fork's reader select, and
restoring the unbounded group read each fail the audit; removing the leftover
check fails the new fork test. Limits: opening the journal is outside the trace
(see the startup characterization); statement counters do not bound bytes, memory
or total work per operation; media files need their own evidence.
