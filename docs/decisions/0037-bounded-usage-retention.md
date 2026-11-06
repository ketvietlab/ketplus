# ADR 0037: Bounded retention of intermediate usage samples

Status: accepted; incremental implementation of #128.

## Decision

The replay projection needs the last `usage.context`, `usage.limits` and
`usage.session` sample of each turn, rather than every intermediate sample
forever. Compact these independently of tool output, image and reasoning
retention. This change does not remove `item.started` events, whose position can
determine transcript row ordering, and never edits message content or item data.

An idle daemon checks at most 128 replay rows per minute in rowid order. The
storage API permits 1–256 rows per call. The scan position advances only after a
successful transaction, wraps at the end and starts again after restart. Rows
that are recent or unacknowledged are therefore reconsidered on later sweeps.
This bounds the outer scan and mutation batch, not the total wall-clock cost of
the indexed eligibility checks within an unusually large turn.

A usage row can be removed only when:

- A later sample of that same event type exists in the same session, generation
  and turn. The last sample is never removed.
- The session's current generation matches and its state is ready, failed,
  interrupted or exited. The service also skips maintenance while any session
  is active.
- At least 20 distinct newer turn sequences remain. Arithmetic distance between
  sequence numbers is insufficient because retained history can have gaps.
- The turn's last lifecycle event is `turn.completed`, and its last event is
  older than the cutoff (currently fixed at 30 days by the service).
- No raw event of that turn remains in `events`. Raw-event retention already
  applies the minimum acknowledged cursor, so a faster client cannot authorize
  removal of a slower client's data.
- The turn contains no question or approval event. This deliberately preserves
  even resolved requests until a later retention policy owns their lifecycle.
- The row is not a large-event manifest. This pass does not change blob ownership.

Deletion and incrementing each affected projection revision share one SQLite
transaction. Existing cursors remain stable; retained samples are not moved to
other cursors. Window continuations bound to the previous revision fail with
`snapshot_changed`. Raw events, cloud events, last cursor and activity groups are
unchanged. A failed transaction retains both data and the previous scan position.

Maintenance temporarily sets `busy_timeout=0` and restores the previous value on
every exit path. It yields to another writer rather than blocking the daemon for
the connection's normal busy timeout. The existing incremental vacuum pass then
reclaims free pages under its own batch and minimum thresholds.

## Remaining scope

This is usage-sample compaction, not completion of #128. Tool-output trimming,
image/reasoning detail, a safe policy for superseded item starts, configurable
retention, archived-history deletion, size-budget enforcement, storage UI and
warnings remain separate work. No live user journal is compacted by the tests,
and no multi-GB shrinkage or installed-device result is claimed.
