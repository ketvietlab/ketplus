# ADR 0094: Staged session fork copy

Status: accepted; a fork copies one read snapshot in bounded passes.

`session.fork` copied every history row of the source generation in one
synchronous journal transaction after the provider forked its thread. A
temporary Release fixture measured 1,899 ms for 100k raw plus 100k replay rows
with 256 MiB of chunks and 2,952 ms for 300k, blocking the daemon event loop and
every other journal writer (#68).

`beginFork` refuses a destination that exists, is staged or has pending purge
work, records a `session_fork_staging` row and opens a second, query-only SQLite
connection holding one read transaction. The source counters and every copied
row come from that snapshot, so the fork equals the source at that instant even
while the source keeps running, is compacted or is deleted. `continueFork`
copies at most 2,048 rows or 8 MiB per pass (a single larger row is copied
whole) in primary-key keyset order, events and replay rows first, large
payloads before their chunks, so orphan GC never meets a staged payload or
chunk without its parent. TEXT travels as its stored bytes and is cast back, so
malformed UTF-8 is copied exactly as `INSERT ... SELECT` did. Each insert must
affect one row and each finished table must match the snapshot's row count.

`finishFork` removes the staging row, stores the session, sets its start time
and runs the media-ownership preparation (ADR 0068) in one transaction. Until
then the copied rows have no `sessions` row and are never served; a new session
cannot adopt an identity that is staged or awaiting purge. A failed, refused or
abandoned copy turns its staging row into an ADR 0093 purge marker in one
transaction. Opening the journal does the same for every staging row, because
only the daemon owns the journal (`DaemonService` holds its lock before opening
it) and no copy survives a restart. Both paths check the result rather than the
write count: the purge marker must exist and the staging row must be gone, or
the transaction rolls back and the staging row keeps owning the copy.

`SessionHost` runs one pass per event-loop turn and defers 250 ms when another
writer is busy. It rejects a second request for a destination being copied and
cancels the fork with `session_not_found` if the source generation is deleted
before the copy finishes. The synchronous `forkSession` keeps its previous
contract by running the passes to completion and removing anything it staged on
failure.

Release fixture, checkpointed before timing: begin 3–7 ms, finish 0–1 ms and
slowest pass 17 / 73 / 29 ms for 1k / 100k / 300k rows. Moving rows through the
application roughly doubles total copy time (5.7 s for 300k) in exchange for no
long stall. Holding the snapshot delays WAL checkpoint reset for the copy's
duration. No protocol envelope, provider call or UI change is included.
