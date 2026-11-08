# ADR 0055: Session deletion transaction recovery

Status: accepted; failed deletion must not commit or expose partial state.

The local deletion transaction removes history and stores both a tombstone and
provider/cloud cleanup work. Its failed-commit branch returned without rollback.
A deferred-constraint Qt probe reproduced a still-open transaction: a subsequent
BEGIN failed, and reads could see the uncommitted tombstone. Two other probes
ignored either mandatory insert; deletion still reported success. Ignoring a raw
history deletion also left data behind a committed tombstone.

A transaction scope guard now rolls back every unsuccessful exit, including a
failed commit. Only a successful commit disarms it. Tombstone and cleanup-work
inserts must each affect exactly one row. After each existing history-table
DELETE, an identity/generation existence check must find no remaining row before
commit; query errors fail closed. These checks preserve the existing operation
and lifecycle-revision idempotency fences. Retention runs only after commit and
its failure does not turn a committed deletion into a reported failure.

Regressions inject deferred commit failure and ignored tombstone/work/raw-history
mutations. Each must return failure, release the transaction, preserve history
and the assigned conversation number, and leave no tombstone or deletion work.
Removing the fault permits the exact operation to retry successfully; reopen
retains its tombstone and provider targets, while an unrelated session survives.
A different operation cannot reuse the completed deletion identity.

This change does not add a schema, alter command envelopes or make whole-session
deletion asynchronous. The existing bulk deletes and provider-reference checks
still require separate bounds and fail-closed read audits. Previously committed
incomplete state, low disk, process-kill recovery and downgrade compatibility are
not certified by these injected failures. No UI, provider-quota calls, live
journal mutation, installation or deployment is included.
