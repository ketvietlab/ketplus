# ADR 0049: Bounded cleanup of acknowledged delivery events

Status: accepted; physical raw cleanup becomes eventual, ACK acceptance stays durable.

The old ACK transaction checked each large payload against all newly acknowledged
raw JSON, then deleted the entire acknowledged backlog. A Qt fixture with 100,000
raw events and 32 metadata rows took 7,183 ms. This path was separate from ADR 0048's
bounded global orphan GC.

An advancing ACK now commits its monotonic client cursor and an entry in
`journal_ack_gc` together. Missing/ignored queue writes reject the ACK transaction.
An ACK at or behind the stored cursor still performs no write or maintenance.
The reply acknowledges durable delivery progress; it does not promise immediate
physical deletion. Replay transcript rows are unaffected.

After that commit, one bounded raw cleanup pass runs, followed by one ADR 0048 blob
pass on success. Raw cleanup also runs once at reopen and on the existing idle
maintenance timer. A cleanup failure cannot turn a committed ACK into a rejection:
the queue survives for a later pass/restart. Active traffic supplies additional
ACK-driven passes; idle maintenance drains remaining work after sessions settle.
This is not a steady-state disk quota or a guarantee that every backlog drains
while a session remains continuously active.

Each raw pass visits at most eight queued generations and 128 event candidates,
and deletes at most 1 MiB of raw bytes. It re-reads the slowest client's cursor in
its transaction through `client_cursors_generation(session_id,generation,cursor)`;
a newly registered slower client therefore constrains already queued work. Event
selection seeks the primary session/generation/cursor index. No raw JSON is parsed
and there is no candidate-by-backlog transfer search. Blob cleanup has its own
separate 1 MiB budget and keeps raw/replay references protected.

Each generation is read from its earliest remaining raw cursor. Cleanup removes
only a contiguous acknowledged prefix: hitting the byte budget or an unsupported
row preserves that row and its entire following tail. This keeps the raw replay
API's minimum-retained-cursor expiry contract valid; no new interior gaps are made.
An empty eligible scan finishes the queue entry. Unfinished entries rotate to the
end using an AUTOINCREMENT queue ID, so a blocked generation cannot starve others.
Raw deletions and queue rotation are one transaction. Abort/ignored writes/commit
failure roll back both. Busy writers defer immediately and the previous SQLite
busy timeout is restored.

`octet_length(payload)` measures stored TEXT bytes from SQLite record metadata,
including embedded NULs, without loading the payload. This follows SQLite's
[documented column optimization](https://www.sqlite.org/lang_corefunc.html#octet_length).
Non-TEXT and individually oversized raw rows block cleanup of their generation's
remaining tail. Their recovery remains H1.2/H1.5 work. If an externally supplied older
SQLite lacks `octet_length`, ACKs and their queues remain durable, physical raw
cleanup is deferred, and the daemon logs the capability limitation once. The
bundled/tested plugin supports it; no unbounded length-function fallback is used.

The queue/index are additive. The index's one-time creation and migration failure
behavior remain part of H1.2 acceptance. Other command/tombstone/audit sweeps,
cloud-event deletion and explicit session deletion remain separate audit work.

`ketplus_daemon_ack_retention_tests` covers real backlog ACK, row/byte bounds,
actual prepared query plans, slower/new clients, generation isolation, queue
rotation/reopen, embedded NULs, unsupported values/capability absence and busy/fault
retry. Actual SessionHost `session.events_after` regressions preserve the suffix
behind an unsupported head/middle row and verify prefix-expiry recovery. Protocol and transcript-window fixtures preserve existing small-ACK behavior.
No provider calls, live journals, UI implementation, installation or deployment are
part of this change; the plan records final-head testing and independent review.
