# ADR 0062: Bounded ordered recovery after journal append failure

Status: accepted for the local runtime; durable crash recovery remains separate.

SessionHost previously removed a held delta before checking whether append
succeeded. Timer, item-boundary and lifecycle-boundary fault probes lost the
prefix or allowed a later event to commit first. All three probes fail with the
original implementation.

The runtime now retains failed provider events in session order. A failed prefix
precedes later item/lifecycle events; same-item deltas may coalesce only when all
other metadata matches. Retries wait one second after failure and drain at most
one deferred event per session per timer callback. Repeated identical write
errors are logged once until a write succeeds. Normal coalescing keeps its
150 ms / 16 KiB threshold.

The deferred queue admits at most 128 events and 40 MiB per session, 512 events
and 80 MiB per host. Byte accounting uses compact JSON, the event type and 512
bytes of bookkeeping per entry; it is not a measurement of process RSS. The
preexisting coalescer is separate: if the shared budget is full, an already-held
prefix within the ordinary event cap remains there, and new events cannot
replace it. Full queues reject new entries with one warning per episode. An
unsupported oversized event is rejected before admission so it cannot pin the
retry head. Large item admission matches the journal's 32 MiB chunked-item lane.

A retained event is not reported as a durable append. Failed user.message,
turn.queued, session.created and session.provider_changed submissions are not
deferred: their callers must receive the existing journal error without a rejected
command being silently committed later. This does not make the multiple writes
of a whole command atomic.

Deleting a session releases its deferred budget and prevents retry resurrection.
Recovery detaches each entry before publishing its event, and reacquires session
identity after synchronous callbacks; a subscriber may remove the session during
publication. Shutdown attempts to drain available storage, stopping on failure.
There is no on-disk retry spool, crash durability, or guarantee to retain provider
output after a queue limit is reached. Providers are not suspended by this change.

Synthetic fake-provider regressions cover timer/item/lifecycle boundaries,
command rejection, oversized input followed by a valid event, count and byte
limits across sessions, deletion budget release, synchronous subscriber deletion,
shutdown, retry after a real SQLite writer lock, and normal coalescing. The focused
set passes 17/17; the shared-header change additionally requires the full Pro
matrix and independent exact-head review before merge.

No schema, IPC, UI, provider service, release or deployment change. Caller-level
command atomicity, ignored multirow mutations, startup/backfill and crash recovery
remain on H1.3/H2.3. No Windows, live-journal or multi-GB claim is implied.
