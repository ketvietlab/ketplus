# ADR 0063: Verify complete replay cleanup and activity settlement

Status: accepted; multirow append mutations must satisfy their postconditions.

A successful SQL execution did not prove that all superseded replay deltas were
removed or all running activity rows were settled. Fourteen temporary-journal
fault probes reproduce successful appends after BEFORE triggers ignore all or
some target rows. This could retain duplicated replay detail or running activity
after a completed/error turn, while delivery cursors advance.

Within the existing append transaction, replay cleanup now verifies that no
matching assistant/command delta remains for the exact session, generation, item
and turn. The verification shares the delete predicate and bindings. Activity
settlement similarly verifies that neither activity table retains running rows
in the target session/generation/turn. Query failure or an unmet postcondition
returns failure to the caller, rolling back raw events, projections, revisions,
activity state and session counters. Empty target sets remain valid.

Both activity tables gain an additive partial index on session, generation and
turn for running>0. Settlement and its verification therefore seek running rows
in the target turn without scanning settled history. Existing replay item/turn
indexes already serve the replay predicate. This does not bound the number of
running items in one turn or deltas in one completed item; that remains H1.1.

The fault fixture covers assistant/command snapshots, ordinary/chunked assistant
items, activity items/summaries, turn completion/runtime errors and complete or
partial ignored writes. It asserts rollback, transaction release, repair/retry
and reopen. All fourteen added cases fail on old production; the combined focused
append fixture passes 47/47 after the fix. The activity suite passes 10/10,
including a 100,000-row fixture in each table, preservation of a running earlier
turn, parameterized query plans and index recreation after reopening an older
journal. Twenty-five actual production settlements take 8 ms in that fixture;
this observation is not a universal latency guarantee.

No public API, UI or retention-policy change. The indexes require an initial scan
of their tables during upgrade; multi-GB startup, low-space migration, unsupported
counter formats, process interruption and higher-level command atomicity remain
separate acceptance work. No live journal/provider, Windows, install or deployment
verification is implied.
