# 0005: Bounded local transcript windows

Status: accepted
Date: 2026-09-12

## Context

Desktop's compact replay restores every session from cursor zero. Transport
pagination limits a response, not total restore work. Existing
`session.transcript_page` chooses three turns but can truncate inside a giant
turn without an intra-turn continuation. Three turns are not a memory bound.

The compact replay projection is mutable: a completed assistant item deletes
its superseded deltas. Consequently, a cursor ceiling alone cannot promise a
stable historical snapshot across separate page requests. A completion between
requests can otherwise silently remove events from that snapshot.

## Decision

Add **local-only** `session.transcript_window`, independently negotiated through
`session.list.transcript_window_version = 1`. Existing framed IPC version zero,
`session.transcript_page` version one, cloud contracts, encryption, and mobile
routes are unchanged. New clients must not infer support from product version.

An initial request has payload `{ "turn_limit": 3, "event_limit": 128 }`.
`before_cursor` may be omitted or zero. Initial requests must omit
`snapshot_cursor` and `projection_revision`.

The result contains:

| Field | Meaning |
| --- | --- |
| `transcript_window_version` | Exact local window contract version, currently 1 |
| `session_id`, `session_generation` | Required identity/fencing scope |
| `events` | Existing normalized replay event shape, in ascending cursor order |
| `snapshot_cursor` | Initial read's durable event cursor; live begins strictly after it |
| `projection_revision` | Compaction revision for this session/generation |
| `before_cursor` | Exclusive bound to request older events, including within a turn |
| `has_more_before` | At least one older projected event exists |
| `partial_first_turn` | Page starts inside a turn, not at its first event |
| `partial_items` | `{item_id, turn_sequence, first_cursor}` for assistant delta prefixes not loaded |
| `state`, `active_turn_id`, `current_turn_sequence` | Current authoritative runtime lifecycle, not historical event replay |
| `pending_approvals` | Current unexpired broker requests, never reconstructed from historical requests |
| `pending_approvals_has_more`, `pending_approvals_next_id` | Explicit current approval queue continuation |

An older request supplies all three boundaries from the previous result:

```json
{
  "before_cursor": 101,
  "snapshot_cursor": 900,
  "projection_revision": 4,
  "turn_limit": 3,
  "event_limit": 128
}
```

The page seeks backward from that exclusive event cursor, selects at most three
turns, and returns the newest bounded suffix within them. It never automatically
drains the remaining history. Both event count and encoded byte limits apply:
1–128 events, 1–3 turns, at most 512 KiB for events plus partial-item identity
metadata. A page may therefore contain less than three turns. Per-event storage
still has the existing 256 KiB limit; this change does not recover already
rejected oversized provider events.

The page is never described as a complete message when it contains only a
suffix of a streaming item. Clients retain the explicit prefix-not-loaded
state, do not interpret that suffix as complete Markdown, and prepend missing
content by `(session, generation, turn_sequence, item_id)` on user demand.
Completed-item snapshots with authoritative full text are not partial simply
because their earlier deltas were compacted.
Only an assistant completion with a text string (including an explicit empty
string) may delete its deltas. A completion with a missing/non-string text
field retains the prior events and cannot claim an authoritative full snapshot.

### Consistency and bounded access

Each page uses one short SQLite read transaction. No SQLite transaction,
snapshot connection, copied history, or cursor object is held across IPC calls.
Indexed seeks obtain the starting cursor and at most three turn boundaries;
the event query is an indexed cursor range with a hard row limit. No full
conversation `DISTINCT`, `GROUP BY`, JSON parse, or in-memory sort is used to
obtain the newest window.
The schema upgrade creates one additional item/turn/cursor index once. Index
creation can scan existing projected rows during daemon migration; it is not
repeated for each conversation/window read.

Projection compaction increments a durable revision in the same transaction
that removes assistant deltas. Removal is scoped to the item's turn as well
as session, generation and item ID. An older request with a changed revision
returns `snapshot_changed` with **no partial success payload**. The client must
invalidate that window and request a fresh initial window; it must not advance
its old continuation or treat missing rows as an empty history. Appending new
events without projection deletion does not invalidate older-page cursors.

This deliberately conservative revision fence can invalidate older pages when
an unrelated item in the same session completes. It avoids false snapshot
claims without copying an entire journal or retaining unbounded historical
projection versions. A future immutable revisioned canonical store may narrow
invalidation, but is not assumed by this API.

Clients establish the live subscription before obtaining the initial snapshot,
buffer/epoch-fence concurrent deliveries, then replay or apply events strictly
after `snapshot_cursor`. History loading must not advance the live cursor or
replay historical lifecycle/approval events as current state. On reconnect,
cursor expiry requires a fresh bounded snapshot, not a replay from zero.
Live local event envelopes now carry the existing durable `turn_sequence` too;
clients use the same turn/item identity for live and historical rows.
The local client preserves the daemon's structured error payload alongside its
error string so callers can distinguish `snapshot_changed` from a retryable
transport failure. Callers must still check the error before consuming results.

### Current approvals

The approval broker retains original request details only for active requests.
Resolved/expired requests are not returned, even if an expiry timer has not
run yet. Initial window metadata contains at most 32 requests/256 KiB.
`session.pending_approvals` accepts `{ "after_id": "..." }` and returns the
same queue fields for the remainder, with the same session/generation fencing.
This is a current, changing queue, not a frozen historical snapshot; live
approval notifications and a fresh queue refresh handle concurrent changes.
An individually oversized approval fails explicitly with `approval_too_large`
instead of returning a non-advancing continuation or silently dropping it.
Requests have no default timeout; `expires_at` is an empty string when no
deadline applies. Switching or disconnecting Desktop never resolves a request.
Explicit Stop cancels pending requests for that session and generation after
sending the provider interrupt. Provider exit invalidates its outstanding IDs.
Structured input requests are normalized before they enter this queue. They
contain one to three bounded questions, stable request/question IDs and
revisions, explicit single or multiple selection, stable option IDs and an
explicit custom-answer flag. `question.answer` validates the complete answer
atomically. Defer leaves the request pending and is idempotent for its request
revision; only the first defer appends `question.deferred`. The first valid
answer wins and a late different client receives `already_answered` or
`revision_conflict`.
`approval.resolve` is bound only to execution approvals and cannot resolve an
input request. Invalid provider input fails closed through a JSON-RPC error; no
bounded subset and no empty success answer is synthesized. Accepted, deferred,
and expired input requests append `question.answered`, `question.deferred`, and
`question.expired` respectively. Those lifecycle events carry request ID and revision. The provider transcript
follow-up adds local answer receipts to `question.answered`: non-secret selected
labels/custom answers are retained, secret answers are replaced with `[redacted]`.
Cloud projection still strips the answer receipt. Pending drafts remain memory-only.

## Consequences

- Desktop can initialize metadata-only sessions and hydrate only a selected
  bounded window. The local API itself does not implement Desktop cache eviction
  or renderer virtualization; those require separate client tests.
- Giant turns have lossless event continuation without requiring the UI to load
  all their events at once. Partial giant items remain explicitly partial.
- Old daemon compatibility needs an explicit client fallback; pretending an
  unsupported daemon provides this contract is invalid.
- No command output is added to cloud/mobile. Local replay payload policy is
  unchanged. No provider process or real user journal is required for tests.
- Native frame time, input latency, RSS, and rich Markdown fidelity are not
  proven by journal/query-plan tests.

## Verification

`ketplus_daemon_transcript_window_tests` covers newest-three/older paging, a
1,050-item giant turn, encoded-byte continuation, 200-delta partial item
reassembly, append-only and compaction races, reused item IDs across turns,
restart/raw acknowledgement, current approval pagination, input/scope fences,
and `EXPLAIN QUERY PLAN` on the actual production SQL query constants. Query-plan
checks assert indexed `SEARCH`, no table `SCAN`, and no temporary history sort;
they are not native performance measurements.

## Alternatives considered

- **Keep draining replay from zero:** bounds response size, not startup time or
  model memory; rejected.
- **Use only a turn cursor:** loses the remainder of a giant turn; rejected.
- **Use only a snapshot event ceiling:** ignores mutable compaction; rejected.
- **Copy all history into a snapshot:** makes init cost depend on total history;
  rejected.
- **Serialize a growing assistant snapshot after every delta:** adds full-body
  writes in the token hot path; not required for bounded event windows.
