# ADR 0048: Bounded global large-payload garbage collection

Status: accepted for the global orphan lane; other retention sweeps remain separate.

The backstop called at journal open, after session deletion and every 256 command
results previously scanned all payloads and repeatedly searched pending raw JSON.
An isolated Qt Release fixture with 32 candidates and 100,000 raw rows took about
seven seconds at both open and the command-result threshold. This lane is separate
from ADR 0047's detail compaction and targeted ACK cleanup.

Global GC now uses a persistent singleton `journal_gc_state` with metadata and
chunk rowid cursors. Each pass examines at most 32 metadata rows and 128 chunk
rows. It reads identities and BLOB lengths, not BLOB contents, and deletes at most
1 MiB of combined legacy-blob/chunk bytes. SQL work and payload byte caps are not
hard wall-clock or filesystem-I/O quotas: B-tree updates, fsync and storage latency
still apply. Empty scans wrap a cursor to zero; busy/failing passes never advance.

A generation with any pending raw event is deferred using an indexed existence
lookup, without parsing its backlog. Otherwise an exact session/generation/item/
transfer lookup through the replay expression index protects shared references.
The prepared production queries and actual bound types are tested, including all
four replay keys. Retained candidates advance the cursor and are revisited after
wrap, so an ACK or removed replay reference eventually permits collection.

After removing unreferenced metadata, its chunks may remain physically orphaned
until later passes. Chunk collection checks the exact parent key and the same raw
generation fence. No retained metadata loses its chunks through this lane. Cursor
writes and all row deletions are one transaction; errors, ignored writes and commit
failure roll back the whole pass. The connection's busy timeout is temporarily
zero and restored on every exit. A competing writer defers the pass without turning
opportunistic maintenance into a journal-open error. WAL readers retain snapshots.

The existing once-a-minute idle maintenance callback runs one extra GC pass, only
when no session is active, before free-page reclaim and detail retention. New
commands are therefore not required to finish deferred chunk cleanup. Initialization
also performs one bounded pass and resumes existing cursors after restart.

Legacy monolithic BLOBs over 1 MiB, non-BLOB payload values and chunks larger than
128 KiB are preserved and skipped. Their explicit migration/recovery path remains
H1.2/H1.5 work; this change does not claim comprehensive storage enforcement. The
additive cursor table does not change event/manifest formats or external APIs.
Targeted ACK/detail cleanup, command/tombstone/audit retention and session deletion
are not made bounded by this decision and remain audit items in H1.1/H1.3.

`ketplus_daemon_garbage_collection_tests` covers real reopen/command-triggered paths,
300,000 same-item replay rows, candidate/byte budgets, shared references and
generation isolation, cursor restart/wrap, unsupported rows, abort/ignored-write/
commit faults, busy writers and WAL readers. Fixtures are synthetic and temporary;
no user journal or provider account is used. See the backend hardening plan for
final-head matrix, review and remaining acceptance gates.
