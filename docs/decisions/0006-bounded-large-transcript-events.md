# ADR 0006: Bounded large transcript events

Status: implemented for daemon persistence, local Desktop consumption, and the
opaque Control API event upload path. Mobile consumption still requires the
handoff below.

## Decision

A normalized `item.started` or `item.completed` payload larger than the existing
256 KiB event limit is represented as version 1 bounded chunks followed by one
terminal manifest. This applies only to transcript items with an identity and
never to `commandExecution`: command events continue through the command-only
cloud projector, so command output cannot enter a chunk.

The daemon accepts at most 32 MiB of compact UTF-8 JSON for one logical item. This
allows an 8 MiB provider body to retain both its nested item and the normalized
top-level compatibility field without becoming unbounded. It
stores the complete bytes and all event rows in one SQLite transaction. Cloud
chunks use 96 KiB raw slices so JSON/base64 plus AES-GCM/base64 envelope overhead
remains below the existing 256 KiB event cap. Chunk rows are uploaded through
the existing opaque `/v2/events:append` contract, but are excluded from the
local compact transcript projection and are never rendered.

Each raw chunk has `event_type: "transcript.large_chunk"` and this payload:

```json
{
  "large_event_chunk_version": 1,
  "transfer_id": "uuid",
  "item_id": "provider-stable-item-id",
  "revision": 88,
  "chunk_index": 0,
  "chunk_count": 86,
  "offset": 0,
  "total_bytes": 8388710,
  "digest_sha256": "64-lowercase-hex",
  "data": "base64url-without-padding"
}
```

The terminal event retains the original `event_type` and carries a compact
manifest instead of the original payload:

```json
{
  "item_id": "provider-stable-item-id",
  "item_type": "agentMessage",
  "large_event": {
    "version": 1,
    "transfer_id": "uuid",
    "item_id": "provider-stable-item-id",
    "revision": 88,
    "event_type": "item.completed",
    "encoding": "json-utf8",
    "total_bytes": 8388710,
    "chunk_count": 86,
    "chunk_bytes": 98304,
    "digest_sha256": "64-lowercase-hex"
  }
}
```

`session.transcript_window` marks such a manifest in `partial_items`; it does
not claim complete content. Desktop then reads
`session.transcript_large_event` from offset zero in independently bounded
128 KiB fragments. Only after contiguous coverage, terminal-length agreement,
SHA-256 verification, UTF-8 JSON decoding, and item-identity validation does it
replace the manifest with the original payload. Missing content, a gap,
reordering, mixed metadata, conflicting duplicate, stale generation, or changed
window epoch fails the whole page visibly. Exact duplicate fragments are a
no-op. No partial `item.completed` reaches the presentation model.

The durable payload is deleted transactionally with its session. A single
fragment request and a single in-memory reassembly are bounded; the full local
history remains authoritative and does not receive the cloud 24-hour TTL.

## Mobile/backend handoff

The production Control API event schema already accepts the encrypted chunk and
manifest as opaque event content; no database column is required merely to
store them. That is not sufficient for a mobile transcript page to be complete.
Before mobile enables this representation it must:

1. advertise `transcript_large_event_version: 1` only when it implements these
   exact fields and limits;
2. hide `transcript.large_chunk` from the transcript UI and cache fragments by
   owner/workspace/machine/session/generation/transfer/item/revision scope;
3. apply the same exact-duplicate, conflicting-duplicate, offset, length,
   terminal, digest, decoded-schema, epoch and cancellation fences as Desktop;
4. keep the manifest incomplete until every byte is verified, then publish one
   original logical event; and
5. use a bounded authenticated hot-fragment fetch while all referenced chunks
   remain inside the 24-hour cloud replica. Its opaque continuation must bind
   scope, generation, transfer, item, revision, digest, offset, credential
   epoch, snapshot and expiry, return explicit 410 on expiry, and never return a
   manifest as complete coverage without the body; and
6. use an encrypted, idempotent, observe-only `transcript.restore_window`
   mailbox command for content absent from the hot replica. The backend cannot
   synchronously call an outbound-only daemon. Mobile queues a scoped request;
   the daemon polls, claims a bounded lease, reads its authoritative SQLite
   history and re-uploads a bounded page/chunks under a new response cursor;
   mobile then resumes from that response. Request ID/idempotency key, owner,
   workspace, machine, session, generation, credential epoch, snapshot,
   requested byte cap and deadline are authenticated and encrypted. Claim
   token/attempt fencing, at-least-once duplicate handling, cancellation,
   terminal-state CAS, response digest, TTL and explicit unavailable/410 states
   prevent a late daemon or stale generation from reviving an expired request.
   This is the older-history mailbox already proposed by Backend, not an inbound
   proxy or a direct request from cloud to the Mac.

Item 6 is now implemented on the Control API and outbound daemon path. Restore
pages are split into 48 KiB raw JSON fragments, independently encrypted and
signed under a request-scoped response cursor; the final fragment binds the
SHA-256 digest of the complete local page. The daemon uploads the page before a
compact terminal acknowledgement and never places the page itself in the
64 KiB command result. A fresh mailbox request names a snapshot at or before
the SQLite journal head and is served as of that cursor, never with a later
event. The relay learns the head from heartbeats, so during a running turn its
snapshot always trails the journal; requiring equality left a running
conversation unrestorable on the phone (`restore_window_unavailable`). Newer
events reach the phone live. A compaction after the snapshot drops only
deltas whose completed item, carrying the full text, lies after it. The daemon
binds the current projection revision in the same read transaction that
selects the page, including when `before_cursor` is nonzero; mobile never
guesses or supplies that local revision. A snapshot after the head returns
`invalid_page` with no page. Ordinary local IPC continuations remain strict and require their
issued snapshot and projection revision, while large-event continuations retain
their separate immutable revision/digest/identity/offset fence. The backend
keeps the opaque response only until the request deadline (at most ten minutes)
and returns 410 after expiry,
cancellation or explicit unavailability.

A mailbox page containing a `large_event` manifest is only the root of a
bounded continuation chain. The next command keeps the selector opaque in its
encrypted body as exactly:

```json
{"large_event":{"version":1,"transfer_id":"...","item_id":"...","revision":1,"digest_sha256":"...","total_bytes":1,"offset":0}}
```

Its clear authenticated routing tuple is the parent request ID, monotonically
increasing continuation sequence and the parent's terminal response digest;
the canonical root tuple is `"", 0, ""`. The tuple is also part of the daemon
response-cache scope. The daemon rejects root/continuation shape mixing and
validates the selector against the retained item plus the requested
session/generation and snapshot. A child returns the existing
`session.transcript_large_event` result (including `next_offset` and
`complete`) through one or more 48 KiB encrypted mailbox fragments. The local
range is at most 128 KiB and shrinks for a smaller requested encrypted-byte cap.
Same-key lease retries upload byte-identical ciphertext; a machine-key rotation
creates a freshly encrypted cache entry.

The remaining mobile work is mechanical but security-critical: emit the exact
request AAD extension, consume the response endpoint with operation-epoch and
cursor fencing, reassemble exact offsets, validate total length and terminal
digest, then pass the verified page through the existing transcript projection.

Until items 5–6 ship, local Desktop round-trip is lossless and cloud upload is
bounded/durable, but a cold mobile client must fail the item as unavailable
rather than display a false completion.
