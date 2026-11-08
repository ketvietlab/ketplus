# ADR 0052: Bounded completed deletion work

Status: accepted; physical completion cleanup is eventual.

Acknowledging a cloud deletion or completing one provider target previously ran
an unbounded DELETE of every completed job. A synthetic Qt backlog of 100,000
completed jobs took 413 ms to acknowledge one unrelated deletion and removed
100,001 rows. Retaining completed rows without changing the reader would instead
leave a backlog-sized pending scan and round-robin sort on the heartbeat path.

Completion now durably updates only the identified session/generation. Cloud
receipts require the exact nonempty operation ID. Provider completion requires
nonempty provider/thread IDs and an array of targets; malformed JSON cannot be
interpreted as an empty completed list. Compare-and-set updates preserve the
original stored record if another writer changes it. An ignored update returns
failure, rather than claiming durable success.

Completed jobs are logically absent from `deletionWork` immediately, while a
sixth metadata-retention lane reclaims their storage. Only `cloud_acked=1` with
the canonical text `provider_targets='[]'` is eligible. The lane shares the
existing per-table 256 candidate/deletion and 1 MiB column-byte limits, durable
cursor, transaction rollback, busy deferral and unsupported-row preservation
(ADR 0050). It runs at reopen and idle maintenance, independently of ACK/provider
completion. There is no age requirement for already completed work.

Pending reads use additive state indexes: an expression index for either pending
gate and a cloud-ack index for cloud-only reads. Two indexed rowid ranges implement
the existing suffix-then-prefix rotation with one shared limit (1 through 256).
They neither sort completed backlog nor scan it to find pending rows. Tests use
the exact prepared production statements, including bound rowid/limit parameters,
and require both state and rowid index constraints without a temporary sort.

Existing columns and completion protocol are unchanged. Additive indexes require
one initial build; startup/index and low-space acceptance remain in H1.2. This
change bounds completed-job GC and page lookup by row count/work; it does not
claim a byte limit on pending JSON decoding or one explicit completion record.
Malformed or individually oversized records require the separate recovery work.
Explicit session deletion, cloud-cache invalidation and tombstone enumeration
are separate H1.1/H1.3 audits. No UI or provider-quota behavior is added.
