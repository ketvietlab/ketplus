# ADR 0053: Separate cloud cache receipts from invalidation

Status: accepted; ACK cache reclamation is eventual.

Cloud cleanup previously deleted every envelope through a cursor, both after an
upload ACK and when recovering a cached envelope with wrong projection, identity,
turn or key scope. A synthetic Qt 100,000-row cache (1-KiB payloads) took 269 ms
in one delete. A raw cloud-client cursor is not sufficient authority for deferred
cache cleanup: it can outlive an account/workspace reset and incorrectly authorize
removal of newly regenerated ciphertext at the same session/generation/cursor.

An additive `cloud_events.upload_acked` integer column defaults to zero, including
legacy rows. Before dispatch, the journal verifies the batch against the exact
cached session/generation/cursor and encrypted content, then atomically clears
these rows' receipt flags. This preserves cached ciphertext across failed uploads
and retries, even if the same envelope was accepted in a previous workspace.
After an upload is accepted and local progress is stored, only the captured sent
envelopes through the accepted cursor can gain a receipt. A missing or regenerated
cache row cannot inherit an older ciphertext's receipt. Stale account-epoch,
workspace or machine callbacks release their in-flight slot without writing
progress/receipts/backoff or restarting upload.

Each receipt/pending transaction checks at most 100 rows and 1 MiB of stored
column bytes using metadata before loading JSON. Ignored/aborted writes, cache
mismatch before dispatch, oversized/unsupported rows and failed commits return
failure and roll back the whole batch. A changed cache after dispatch is skipped
when recording the older receipt, preserving the replacement. Busy work returns
immediately and restores the original timeout. These bounds cover the existing
100-event/768-KiB upload batch; oversized legacy input fails closed for recovery.

Cache reclamation is a separate metadata-retention lane: at most 256 candidates/
deletions and 1 MiB of stored column bytes per pass, with a durable rowid cursor.
Only supported rows with `upload_acked=1` are eligible. Successful upload, reopen
and idle maintenance retry unfinished cleanup. There is no backlog scan to find
eligible rows, and raw/replay ACK fences remain unchanged. Legacy unmarked cache
is conservatively retained; it needs a verified future receipt or separate recovery,
not inference from an old account cursor.

Invalidation synchronously discards only the exact cached identity, and must
succeed before re-encryption or reconciliation is reported. Its indexed metadata
lookup checks supported types and a 1-MiB column-byte bound without hydrating the
payload. Missing rows are idempotent success. Unsupported/oversized entries or a
missing byte-length function cannot be discarded; upload fails closed until
recovery handles them. Valid earlier envelopes retain their ciphertext.

ACK maintenance inherits ADR 0050 transaction/cursor rollback, capability/busy
deferral, one empty-tail wrap and unsupported-row fairness. Limits are per lane,
not a hard disk quota. Cache is derived data, not a replay-history authority.

Tests cover backlog/byte/reopen bounds, stale cursors and regenerated ciphertext,
legacy migration, pending/receipt transitions and partial ACKs, exact invalidation,
row/byte receipt limits, ignored mutations/commit rollback, busy restoration and
production prepared query plans. Transport fixtures check failed policy/turn
invalidation and an old upload callback delivered after sign-out.

Per-entry ordinary cache read/JSON limits, malformed empty-object recovery,
legacy unmarked-cache recovery, explicit session deletion, low-space migration
and representative multi-GB acceptance remain separate checks. This package does
not claim Windows, live-journal, deployment or installation verification.
