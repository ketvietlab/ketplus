# 0133: Semantic routing receipt lifetime and isolation

Status: proposed (implementation under review)
Date: 2026-11-17

## Context

The host recipe in ADR 0130 proves a committed delta. Binding that proof to the
whole team's activity revision makes unrelated notes and ordinary background
delivery invalidate it, including during deferred verification. That transport
revision is still necessary for optimistic concurrency when recording a decision.
The previous global hash-table eviction also supplies no deterministic team quota.

## Decision

Keep the exact team revision at request admission and leader decision time.
Separately hash the task's semantic document, non-note history, relevant decision
and dependency state, policy/settings and membership fences, and exact repository,
integration, worktree, branch, base and full head. An unchanged round-zero head
report is metadata; reviewed baselines and conflicting head reports remain bound.
The stored proof retains its original task revision. Only after comparing that
binding and revalidating the live clean checkout may the host project the current
task revision for the coordinator. This projection is not caller input or a new
permission. A recorded routing decision changes the semantic document: its audit
does not become a reusable authorization token.

Use a monotonic ten-minute lifetime. Prune all expired entries before admission,
cap each team at 32 and the host at 128, and evict only that team's oldest entry
by insertion order. A full store with no owned slot rejects admission. Existing
IDs cannot overwrite or refresh receipts. Restart discards the in-memory store.
Lookup prunes expiry as well as checking the binding and checkout.

Each deferred completion passes its reply through its synchronous authenticated
broker continuation; remove unconsumed replies on return. Simultaneous identical
requests remain independent, including busy and rejected results. Do not weaken
actor generations, membership, required checks, replacement-object disabling,
submodule visibility, or the real producer boundary.

## Verification

Use actual Git producer/store and authenticated active-team fixtures. Cover
unrelated task notes, developer result notes, in-flight task/settings mutations,
duplicate requests, exact checkout and role fences, expiry boundaries, deterministic
team eviction, global saturation and duplicate-ID nonrenewal. Preserve first-run
failures. Shared receipt headers require the full Pro build and suite. These
offline fixtures grant no user approval and execute no live provider calls.
