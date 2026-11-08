# ADR 0054: Bounded explicit attachment cancellation

Status: accepted; logical cancellation is immediate, physical cleanup is eventual.

The expiry sweep was bounded by ADR 0051, but explicit cancellation of a current
upload still deleted every matching chunk synchronously. A synthetic Qt upload
with 640 chunks of 32 KiB removed all 20 MiB in one transaction. Unknown parent
states and an oversized manifest were also deleted. This path runs on the daemon
event loop and requires its own work and preservation boundaries.

Cancellation now seeks the exact session/generation/attachment/revision manifest,
reads only supported metadata and reserves its conservative column-byte cost
(including possible status growth) in a shared 1-MiB mutation budget. Unknown,
unsupported or oversized manifests fail unchanged. Missing/stale revisions and
already retiring uploads remain idempotent successes.

The chunk read seeks the identity's primary index and takes at most 128 candidates
ordered by chunk index. Revision is checked after this limit: revision is not a
key of that index, and filtering it inside SQL could scan an arbitrarily large
other-revision backlog before reaching 128 matching rows. Tests inspect the actual
prepared production query and exercise 100,000 other-revision candidates.
Chunk content is never hydrated to measure its length. Revision is projected as
an integer or NULL; unsupported TEXT/BLOB revisions are never materialized before
the type guard. The non-STRICT SQLite column can contain such malformed values,
so limiting rows alone would still allow large read-time memory amplification.
Unsupported or individually
oversized chunks are retained; supported requested-revision chunks are removed
only within the remaining byte budget. The budget covers conservatively measured
column bytes, not SQLite page/WAL overhead.

An indexed existence check across every revision determines the final mutation.
If chunks remain, the manifest becomes `expiring`; otherwise it is deleted. This
marker makes cancellation effective immediately and reserves the identity until
existing bounded expiry maintenance drains every old chunk. A retry cannot adopt
stale data or release the marker early. A small upload still cleans up immediately.
Unknown/oversized retained chunks may require separate recovery; cancellation does
not silently discard them or promise that their space will eventually be reclaimed.

The selected deletions and final manifest mutation commit together. Every mutation
must affect exactly one row. Ignored/aborted writes and commit failures roll back
and release the transaction; writer contention fails immediately and restores the
prior busy timeout. No schema, command envelope, quota or UI behavior is changed.
Missing byte-length capability also fails without mutation. The partial-retirement
downgrade limitation in ADR 0051 still applies and belongs to migration/release
acceptance, not this focused implementation.

Regressions cover row/byte bounds, current and stale revisions, unknown/oversized
parents, retained oversized chunks, ID reuse, eventual drain, busy writers,
ignored writes and failed commits. Mutations raising either budget or moving the
revision filter into SQL must fail. Tests use synthetic journals and fake providers;
no live journal, provider quota, installation or deployment is involved.
