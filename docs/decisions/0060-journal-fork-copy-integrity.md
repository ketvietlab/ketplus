# ADR 0060: Fork preserves complete history and destination ownership

Status: accepted; a fork commits a complete copy into a new destination or fails.

SQLite may execute a history-copy statement while triggers ignore some or all
insertions. The original fork path returned success with incomplete delivery,
replay, activity or large-payload storage, or an ignored start timestamp. Eleven
synthetic cases reproduce this. Deferred-constraint commit failure already rolls
back correctly and remains covered.

Within the existing transaction, each history table's source row count is compared
with the affected-row count of its copy. The timestamp identity update must affect
one row. Any mismatch rolls back the destination, context and catalog changes with
all preceding copies. Legitimately empty source tables still copy zero rows.
COUNT uses the same session/generation filter and transaction snapshot as INSERT;
it adds an index walk, not a payload copy. This does not make whole-history fork
bounded or asynchronous; large-fork scheduling remains a separate work-bound audit.

The destination must also be absent in the transaction before session upsert.
The host's early check precedes an asynchronous provider call and cannot reserve
an ID. Two storage probes reproduce replacement of an existing destination or a
self-fork with empty source history. A fake-provider gate reproduces another start
command claiming the destination while the fork response is pending. The guarded
fork fails without replacing that conversation or its provider/thread/history.
Existing tombstone refusal remains in upsert; a regression preserves it.

Tests compare all copied table values, source/unrelated records and context/catalog
state, verify transaction release after faults, then repair/retry and reopen.
An empty-history control remains accepted. The delayed-provider probe uses a local
fake executable and no provider quota. With canonical worktree paths matching
production resolution, removing the destination guard makes the race probe fail:
the delayed fork incorrectly succeeds. The initial noncanonical harness was masked
by a different context guard and was corrected before accepting this evidence.

No schema, public interface, CMake or UI changes. Source counter storage classes,
whole-history work bounds, startup/migration and process-interruption recovery
remain separate hardening items. No live journal/provider, Windows, multi-GB,
packaging, installation or deployment acceptance is implied.
