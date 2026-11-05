# Backend And Core Review Notes

R00-B historical plan review below. R00-C has now reviewed real handoffs at
exact revisions in [cross-team-interface-register.md](cross-team-interface-register.md)
and [owner-review-sources.json](owner-review-sources.json). Earlier statements
about absent owner handoffs describe only the R00-B snapshot.

## Backend Contract Proposal

Status: reviewed as proposal, not approved API.

Provenance: this review covers PR #17 source revision `37a14562a11b6ec9fb7f137e9e93c95af464bc8d`, specifically `tasks/desktop-refactor/02-api-alignment.md`. No Backend owner handoff artifact, OpenAPI diff, fixture bundle, query plan, migration, or test result was received in PR #18/R00-B.

Strong points:

- Keeps Desktop on local daemon IPC and mobile on Control API, preserving the service boundary.
- Treats `202` command acknowledgement as queued/accepted command state, not turn completion.
- Separates transcript page coverage from raw event cursor acknowledgement.
- Requires encrypted cloud projection to omit command output before persistence.
- Calls out missing archive/delete/restore lifecycle instead of inferring endpoints from the mock.

Items that must be resolved before UI consumers:

- Freeze identity scope: subject/account, workspace, machine, repository, worktree, conversation, session generation, turn, and item.
- Define lossless continuation for large turns and large items within IPC/cloud encoded byte budgets.
- Specify canonical item IDs, order keys, revisions, unknown-kind fallback, and stale conflict handling.
- Add lifecycle metadata and tombstone retention that outlives 24-hour transcript content TTL enough to prevent resurrection.
- Provide DB-side keyset pagination, retention cleanup batches, EXPLAIN evidence, and fixture hashes.
- Define old-client/old-daemon negotiated fallback without silent full-history replay.

Review outcome:

- `reviewed`: C01 to C05 are the right contract areas.
- `blocked`: API is not approved until Backend owner produces schema, fixtures, migration/query evidence, and compatibility tests.

## Core / Transcript Performance Findings

Status: reviewed as risk assessment, not measured in this handoff.

Provenance: this review covers PR #17 source revision `37a14562a11b6ec9fb7f137e9e93c95af464bc8d`, specifically `tasks/desktop-refactor/04-pixel-performance-gates.md`. No Core owner handoff artifact, workload generator output, release benchmark, or focused runtime test result was received in PR #18/R00-B.

Findings accepted for planning:

- Virtual delegates do not bound source data, parsed Markdown, media decode, SQLite work, or replay payloads.
- Eager attach/full-history assumptions are incompatible with first paint and bounded memory goals.
- Current truncation concepts are insufficient without continuation and covered ranges.
- Stable reading anchors need item ID plus pixel offset and follow-tail state, not row index alone.
- Streaming requires coalesced updates, final flush, and no full model reset on every delta.

Performance gates that remain proposal:

- Active transcript window budget: up to 8 MiB source or 1,000 items, whichever is reached first.
- Aggregate source cache: up to 64 MiB.
- Decoded media cache: up to 64 MiB.
- Rich layout documents: viewport plus overscan.
- Warm transcript views: up to 8, matching current cache direction.

Review outcome:

- `reviewed`: performance risks and fixture categories are credible blockers.
- `blocked`: no performance target is approved until R00/R14 collect release-build baseline data on the locked environment.
